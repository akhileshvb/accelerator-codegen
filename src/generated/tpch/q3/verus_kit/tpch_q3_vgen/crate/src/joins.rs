use vstd::prelude::*;
use std::sync::Arc;
use crate::maps::*;
use crate::gen_spec::*;
use verus_common::sums::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub type Counts = Vec<(i64, i128)>;
pub type Groups = Index<Counts>;
pub type Dates = Index<Groups>;
pub type JoinIndex = Index<Dates>;
pub type JoinWork = Work<Work<Work<Counts>>>;

pub open spec fn customer_count(db: Db, key: int, segment: Seq<u8>) -> int {
    sum(db.customer, |row: CustomerRow|
        if row.c_custkey == key && row.c_mktsegment == segment { 1int } else { 0 })
}

pub open spec fn join_count(db: Db, segment: Seq<u8>, date: int, key: int, priority: int) -> int {
    sum(db.orders, |row: OrdersRow|
        if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
            customer_count(db, row.o_custkey, segment)
        } else { 0 })
}

pub open spec fn count_weight(key: i64) -> spec_fn((i64, i128)) -> int {
    |row: (i64, i128)| if row.0 == key { row.1 as int } else { 0 }
}

pub open spec fn count_at(rows: Seq<(i64, i128)>, key: i64) -> int {
    sum(rows, count_weight(key))
}

pub open spec fn counts_ok(rows: Seq<(i64, i128)>) -> bool {
    &&& rows.len() > 0
    &&& forall|position: int| 0 <= position < rows.len() ==> (#[trigger] rows[position]).1 > 0
    &&& forall|first: int, second: int| 0 <= first < second < rows.len() ==>
        (#[trigger] rows[first]).0 != (#[trigger] rows[second]).0
}

pub open spec fn counts_unique(rows: Seq<(i64, i128)>) -> bool {
    forall|first: int, second: int| 0 <= first < second < rows.len() ==>
        (#[trigger] rows[first]).0 != (#[trigger] rows[second]).0
}

pub open spec fn work_count(work: Work<Counts>, key: i64, segment: i64) -> int {
    if work.data@.contains_key(key) { count_at(work.data@[key]@, segment) } else { 0 }
}

pub open spec fn counts_work_ok(work: Work<Counts>) -> bool {
    work_complete(work) && forall|key: i64| (#[trigger] work.data@.contains_key(key)) ==>
        counts_unique(work.data@[key]@)
}

pub open spec fn c_index(index: Index<Counts>, key: i64, segment: i64) -> int {
    match index.at(key) { Some(rows) => count_at(rows@, segment), None => 0 }
}

pub open spec fn c_overlay(base: Index<Counts>, delta: Run<Counts>, key: i64, segment: i64) -> int {
    match overlay_at(base, delta, key) { Some(rows) => count_at(rows@, segment), None => 0 }
}

pub open spec fn h_work(work: JoinWork, segment: i64, date: i32, key: i64, priority: i64) -> int {
    if !work.data@.contains_key(segment) { 0 }
    else {
        let dates = work.data@[segment];
        if !dates.data@.contains_key(date as i64) { 0 }
        else {
            let groups = dates.data@[date as i64];
            if !groups.data@.contains_key(key) { 0 }
            else { count_at(groups.data@[key]@, priority) }
        }
    }
}

pub open spec fn join_index_ok(index: JoinIndex) -> bool {
    &&& index.valid()
    &&& forall|segment: i64| (#[trigger] index.at(segment)) matches Some(dates) ==>
        dates.valid() && dates.size() > 0 &&
        forall|date: i64| (#[trigger] dates.at(date)) matches Some(groups) ==>
            i32::MIN <= date <= i32::MAX && groups.valid() && groups.size() > 0 &&
            forall|key: i64| (#[trigger] groups.at(key)) matches Some(counts) ==> counts_ok(counts@)
}

pub open spec fn date_work_ok(work: Work<Work<Counts>>) -> bool {
    work_complete(work) && forall|date: i64| (#[trigger] work.data@.contains_key(date)) ==>
        i32::MIN <= date <= i32::MAX && counts_work_ok(work.data@[date])
}

pub open spec fn join_work_ok(work: JoinWork) -> bool {
    work_complete(work) && forall|segment: i64| (#[trigger] work.data@.contains_key(segment)) ==>
        date_work_ok(work.data@[segment])
}

pub open spec fn dates_ok(dates: Dates) -> bool {
    dates.valid() && forall|date: i64| (#[trigger] dates.at(date)) matches Some(groups) ==>
        i32::MIN <= date <= i32::MAX && groups.valid() && groups.size() > 0 &&
        forall|key: i64| (#[trigger] groups.at(key)) matches Some(counts) ==> counts_ok(counts@)
}

pub open spec fn date_value(dates: Dates, date: i32, key: i64, priority: i64) -> int {
    match dates.at(date as i64) { Some(groups) => c_index(*groups, key, priority), None => 0 }
}

pub open spec fn date_overlay(base: Dates, delta: Run<Groups>, date: i32, key: i64, priority: i64) -> int {
    match overlay_at(base, delta, date as i64) { Some(groups) => c_index(*groups, key, priority), None => 0 }
}

pub open spec fn date_work_value(work: Work<Work<Counts>>, date: i32, key: i64, priority: i64) -> int {
    if work.data@.contains_key(date as i64) { work_count(work.data@[date as i64], key, priority) } else { 0 }
}

pub open spec fn join_overlay(base: JoinIndex, delta: Run<Dates>, segment: i64, date: i32, key: i64, priority: i64) -> int {
    match overlay_at(base, delta, segment) { Some(dates) => date_value(*dates, date, key, priority), None => 0 }
}

pub open spec fn h_at(index: JoinIndex, segment: i64, date: i32, key: i64, priority: i64) -> int {
    match index.at(segment) {
        None => 0,
        Some(dates) => match dates.at(date as i64) {
            None => 0,
            Some(groups) => match groups.at(key) {
                None => 0,
                Some(counts) => count_at(counts@, priority),
            },
        },
    }
}

pub proof fn lemma_h_at(index: JoinIndex, segment: i64, date: i32, key: i64, priority: i64)
    ensures h_at(index, segment, date, key, priority) ==
        match index.at(segment) {
            None => 0,
            Some(dates) => match dates.at(date as i64) {
                None => 0,
                Some(groups) => match groups.at(key) { None => 0, Some(counts) => count_at(counts@, priority) },
            },
        },
{
}

pub proof fn lemma_join_live(index: JoinIndex, segment: i64) -> (result: (i32, i64, i64))
    requires join_index_ok(index), index.at(segment).is_some(),
    ensures h_at(index, segment, result.0, result.1, result.2) > 0,
{
    hide(sum);
    let ghost dates = index.at(segment).unwrap();
    dates.lemma_nonempty();
    let date = choose|date: i64| (#[trigger] dates.at(date)).is_some();
    assert(i32::MIN <= date <= i32::MAX);
    let ghost groups = index.at(segment).unwrap().at(date).unwrap();
    groups.lemma_nonempty();
    let key = choose|key: i64| (#[trigger] groups.at(key)).is_some();
    let ghost counts = index.at(segment).unwrap().at(date).unwrap().at(key).unwrap()@;
    assert(counts_ok(counts));
    let priority = counts[0].0;
    lemma_count_positive(counts, 0);
    assert(h_at(index, segment, date as i32, key, priority) == count_at(counts, priority));
    (date as i32, key, priority)
}

pub proof fn lemma_count_positive(rows: Seq<(i64, i128)>, position: int)
    requires counts_ok(rows), 0 <= position < rows.len(),
    ensures count_at(rows, rows[position].0) > 0,
{
    hide(sum);
    lemma_sum_ge_term(rows, count_weight(rows[position].0), position);
}

pub fn copy_counts(old: &Counts) -> (result: Counts)
    ensures result@ == old@,
{
    let mut result = Vec::new();
    let mut position = 0;
    while position < old.len()
        invariant position <= old.len(), result@ == old@.take(position as int),
        decreases old.len() - position,
    {
        result.push(old[position]);
        position += 1;
    }
    result
}

pub fn add(rows: &mut Counts, key: i64, change: i128) -> (result: Option<()>)
    ensures result.is_some() ==> (
        (forall|other: i64| #[trigger] count_at(final(rows)@, other) ==
            count_at(old(rows)@, other) + if other == key { change as int } else { 0 })
        && (counts_unique(old(rows)@) ==> counts_unique(final(rows)@))),
{
    let ghost original = rows@;
    let mut position = 0;
    while position < rows.len()
        invariant position <= rows.len(), rows@ == original, original == old(rows)@,
            forall|prior: int| 0 <= prior < position ==> (#[trigger] rows@[prior]).0 != key,
        decreases rows.len() - position,
    {
        if rows[position].0 == key {
            let value = match rows[position].1.checked_add(change) { Some(value) => value, None => return None };
            rows.set(position, (key, value));
            proof {
                if counts_unique(original) {
                    assert forall|first: int, second: int| 0 <= first < second < rows.len() implies
                        (#[trigger] rows@[first]).0 != (#[trigger] rows@[second]).0 by {
                        assert(rows@[first].0 == original[first].0);
                        assert(rows@[second].0 == original[second].0);
                    }
                }
                assert forall|other: i64| #[trigger] count_at(rows@, other) ==
                    count_at(original, other) + if other == key { change as int } else { 0 } by {
                    lemma_sum_update(original, position as int, (key, value),
                        |entry: (i64, i128)| if entry.0 == other { entry.1 as int } else { 0 });
                }
            }
            return Some(());
        }
        position += 1;
    }
    rows.push((key, change));
    proof {
        assert forall|other: i64| #[trigger] count_at(rows@, other) ==
            count_at(original, other) + if other == key { change as int } else { 0 } by {
            lemma_sum_push(original, (key, change),
                |entry: (i64, i128)| if entry.0 == other { entry.1 as int } else { 0 });
        }
    }
    Some(())
}

pub fn positive(rows: Counts) -> (result: Option<Counts>)
    ensures result matches Some(live) ==> (
        (forall|key: i64| #[trigger] count_at(live@, key) == count_at(rows@, key))
        && (forall|position: int| 0 <= position < live.len() ==> (#[trigger] live@[position]).1 > 0)
        && (counts_unique(rows@) ==> counts_unique(live@))),
{
    let mut result = Vec::new();
    let mut position = 0;
    while position < rows.len()
        invariant position <= rows.len(), result.len() <= position,
            forall|key: i64| #[trigger] count_at(result@, key) == count_at(rows@.take(position as int), key),
            forall|entry: int| 0 <= entry < result.len() ==> (#[trigger] result@[entry]).1 > 0,
            forall|entry: int| 0 <= entry < result.len() ==>
                rows@.take(position as int).contains(#[trigger] result@[entry]),
            counts_unique(rows@) ==> counts_unique(result@),
        decreases rows.len() - position,
    {
        if rows[position].1 < 0 { return None; }
        let ghost previous = result@;
        proof {
            assert forall|key: i64| #[trigger] count_at(previous, key) == count_at(rows@.take(position as int), key) by {}
        }
        if rows[position].1 > 0 {
            proof {
                if counts_unique(rows@) {
                    assert forall|entry: int| 0 <= entry < result.len() implies
                        (#[trigger] result@[entry]).0 != rows@[position as int].0 by {
                        let prior = choose|prior: int| 0 <= prior < position && (#[trigger] rows@[prior]) == result@[entry];
                        assert(rows@[prior].0 != rows@[position as int].0);
                    }
                }
            }
            result.push(rows[position]);
        }
        proof {
            assert forall|key: i64| #[trigger] count_at(result@, key) == count_at(rows@.take(position as int + 1), key) by {
                let weight = |entry: (i64, i128)| if entry.0 == key { entry.1 as int } else { 0 };
                lemma_sum_take(rows@, position as int, weight);
                assert(count_at(previous, key) == count_at(rows@.take(position as int), key));
                assert(count_at(rows@.take(position as int + 1), key) ==
                    count_at(rows@.take(position as int), key) + weight(rows@[position as int]));
                if rows@[position as int].1 > 0 {
                    assert(result@ == previous.push(rows@[position as int]));
                    lemma_sum_push(previous, rows@[position as int], weight);
                } else {
                    assert(rows@[position as int].1 == 0);
                    assert(result@ == previous);
                }
            }
            assert(rows@.take(position as int + 1) =~= rows@.take(position as int).push(rows@[position as int]));
            assert forall|entry: int| 0 <= entry < result.len() implies
                rows@.take(position as int + 1).contains(#[trigger] result@[entry]) by {
                if entry < previous.len() {
                    assert(rows@.take(position as int).contains(previous[entry]));
                    let prior = choose|prior: int| 0 <= prior < position && (#[trigger] rows@[prior]) == previous[entry];
                    assert(rows@.take(position as int + 1)[prior] == result@[entry]);
                } else {
                    assert(result@[entry] == rows@[position as int]);
                    assert(rows@.take(position as int + 1)[position as int] == result@[entry]);
                }
            }
        }
        position += 1;
    }
    proof { assert(rows@.take(position as int) =~= rows@); }
    Some(result)
}

pub fn add_work(work: &mut Work<Counts>, key: i64, segment: i64, amount: i128) -> (result: Option<()>)
    requires counts_work_ok(*old(work)),
    ensures result.is_some() ==> counts_work_ok(*final(work)),
        result.is_some() ==> final(work).data@.dom() == old(work).data@.dom().insert(key),
        result.is_some() ==> forall|other_key: i64, other_segment: i64|
        #[trigger] work_count(*final(work), other_key, other_segment) ==
            work_count(*old(work), other_key, other_segment)
            + if other_key == key && other_segment == segment { amount as int } else { 0 },
{
    let ghost original = *work;
    let mut rows = match work.take(key) { Some(value) => value, None => Vec::new() };
    proof { assert(counts_unique(rows@)); }
    if add(&mut rows, segment, amount).is_none() { return None; }
    work.put(key, rows);
    proof {
        assert(work.data@.dom() =~= original.data@.dom().insert(key));
        assert(work.keys@.to_set() =~= original.keys@.to_set().insert(key));
        assert(work_complete(*work));
        assert forall|other_key: i64| (#[trigger] work.data@.contains_key(other_key)) implies
            counts_unique(work.data@[other_key]@) by {
            if other_key != key { assert(work.data@[other_key] == original.data@[other_key]); }
        }
    }
    Some(())
}

pub open spec fn customer_prefix<Element>(work: Work<Counts>, texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>, rows: Seq<Element>, key_of: spec_fn(Element) -> int, text_of: spec_fn(Element) -> Seq<u8>, amount_of: spec_fn(Element) -> int) -> bool {
    forall|key: i64, segment: i64| #[trigger] work_count(work, key, segment) ==
        match texts(segment) {
            None => 0,
            Some(text) => sum(rows, |row: Element| if key_of(row) == key && text_of(row) == text@ { amount_of(row) } else { 0 }),
        }
}

pub open spec fn customer_db_counts(index: Index<Counts>, db: Db, texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>) -> bool {
    forall|key: i64, segment: i64| #[trigger] c_index(index, key, segment) ==
        match texts(segment) { Some(text) => customer_count(db, key as int, text@), None => 0 }
}

pub open spec fn customer_texts_known<Element>(texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>, rows: Seq<Element>, text_of: spec_fn(Element) -> Seq<u8>) -> bool {
    forall|position: int| 0 <= position < rows.len() ==>
        exists|segment: i64| (#[trigger] texts(segment)).is_some() && texts(segment).unwrap()@ == text_of(#[trigger] rows[position])
}

pub proof fn lemma_customer_prefix_step<Element>(
    before: Work<Counts>, after: Work<Counts>,
    old_texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>, texts: spec_fn(i64) -> Option<Arc<Vec<u8>>>,
    rows: Seq<Element>, item: Element, key: i64, segment: i64, amount: int,
    key_of: spec_fn(Element) -> int, text_of: spec_fn(Element) -> Seq<u8>, amount_of: spec_fn(Element) -> int,
)
    requires
        customer_prefix(before, old_texts, rows, key_of, text_of, amount_of),
        customer_texts_known(old_texts, rows, text_of),
        key_of(item) == key, amount_of(item) == amount,
        texts(segment).is_some(), texts(segment).unwrap()@ == text_of(item),
        forall|id: i64| (#[trigger] old_texts(id)).is_some() ==> texts(id) == old_texts(id),
        forall|first: i64, second: i64| first != second &&
            (#[trigger] texts(first)).is_some() && (#[trigger] texts(second)).is_some() ==>
            texts(first).unwrap()@ != texts(second).unwrap()@,
        forall|other_key: i64, other_segment: i64| #[trigger] work_count(after, other_key, other_segment) ==
            work_count(before, other_key, other_segment)
            + if other_key == key && other_segment == segment { amount } else { 0 },
    ensures customer_prefix(after, texts, rows.push(item), key_of, text_of, amount_of),
        customer_texts_known(texts, rows.push(item), text_of),
{
    assert forall|other_key: i64, other_segment: i64| #[trigger] work_count(after, other_key, other_segment) ==
        match texts(other_segment) {
            None => 0,
            Some(text) => sum(rows.push(item), |row: Element| if key_of(row) == other_key && text_of(row) == text@ { amount_of(row) } else { 0 }),
        } by {
        if let Some(text) = texts(other_segment) {
            let weight = |row: Element| if key_of(row) == other_key && text_of(row) == text@ { amount_of(row) } else { 0 };
            if old_texts(other_segment).is_none() {
                assert forall|position: int| 0 <= position < rows.len() implies weight(#[trigger] rows[position]) == 0 by {
                    let prior_segment = choose|prior_segment: i64| (#[trigger] old_texts(prior_segment)).is_some() &&
                        old_texts(prior_segment).unwrap()@ == text_of(rows[position]);
                    assert(texts(prior_segment) == old_texts(prior_segment));
                }
                lemma_sum_zero(rows, weight);
            }
            assert(text_of(item) == text@ <==> other_segment == segment);
            lemma_sum_push(rows, item, weight);
        }
    }
    assert forall|position: int| 0 <= position < rows.push(item).len() implies
        exists|id: i64| (#[trigger] texts(id)).is_some() && texts(id).unwrap()@ == text_of(#[trigger] rows.push(item)[position]) by {
        if position < rows.len() {
            let id = choose|id: i64| (#[trigger] old_texts(id)).is_some() && old_texts(id).unwrap()@ == text_of(rows[position]);
            assert(texts(id) == old_texts(id));
        } else { assert(texts(segment).unwrap()@ == text_of(rows.push(item)[position])); }
    }
}

pub fn change(work: &mut JoinWork, segment: i64, date: i32, key: i64, priority: i64, amount: i128) -> (result: Option<()>)
    requires join_work_ok(*old(work)),
    ensures result.is_some() ==> join_work_ok(*final(work)),
        result.is_some() ==> forall|other_segment: i64, other_date: i32, other_key: i64, other_priority: i64|
        #[trigger] h_work(*final(work), other_segment, other_date, other_key, other_priority) ==
        h_work(*old(work), other_segment, other_date, other_key, other_priority)
        + if other_segment == segment && other_date == date && other_key == key && other_priority == priority {
            amount as int
        } else { 0 },
{
    if amount == 0 { return Some(()); }
    let ghost original = *work;
    let mut dates = match work.take(segment) { Some(value) => value, None => Work::new() };
    let ghost original_dates = dates;
    let mut groups = match dates.take(date as i64) { Some(value) => value, None => Work::new() };
    let ghost original_groups = groups;
    if add_work(&mut groups, key, priority, amount).is_none() { return None; }
    let ghost changed_groups = groups;
    dates.put(date as i64, groups);
    proof {
        assert(dates.data@.dom() =~= original_dates.data@.dom().insert(date as i64));
        assert(dates.keys@.to_set() =~= original_dates.keys@.to_set().insert(date as i64));
        assert(date_work_ok(dates));
    }
    work.put(segment, dates);
    proof {
        assert(work.data@.dom() =~= original.data@.dom().insert(segment));
        assert(work.keys@.to_set() =~= original.keys@.to_set().insert(segment));
        assert(join_work_ok(*work));
        assert forall|other_segment: i64, other_date: i32, other_key: i64, other_priority: i64|
            #[trigger] h_work(*work, other_segment, other_date, other_key, other_priority) ==
            h_work(original, other_segment, other_date, other_key, other_priority)
            + if other_segment == segment && other_date == date && other_key == key && other_priority == priority {
                amount as int
            } else { 0 } by {
            if other_segment == segment && other_date == date {
                assert(work_count(changed_groups, other_key, other_priority) ==
                    work_count(original_groups, other_key, other_priority)
                    + if other_key == key && other_priority == priority { amount as int } else { 0 });
            }
        }
    }
    Some(())
}

pub fn finish_dates(base: &Dates, mut work: Work<Work<Counts>>) -> (result: Option<Dates>)
    requires dates_ok(*base), date_work_ok(work),
    ensures result matches Some(dates) ==> dates_ok(dates),
        result matches Some(dates) ==> forall|date: i32, key: i64, priority: i64|
            #[trigger] date_value(dates, date, key, priority) ==
                date_value(*base, date, key, priority) + date_work_value(work, date, key, priority),
{
    let ghost original = work;
    let mut delta = Run::new();
    let mut position = 0;
    while position < work.keys.len()
        invariant position <= work.keys.len(), run_ok(delta), dates_ok(*base), date_work_ok(original),
            work.keys@ == original.keys@, work_directory(work),
            work.data@ == original.data@.remove_keys(original.keys@.take(position as int).to_set()),
            delta.model().dom() == original.keys@.take(position as int).to_set(),
            forall|date: i32, key: i64, priority: i64| #[trigger] date_overlay(*base, delta, date, key, priority) ==
                date_value(*base, date, key, priority) +
                    if original.keys@.take(position as int).contains(date as i64) { date_work_value(original, date, key, priority) } else { 0 },
            forall|date: i64| (#[trigger] overlay_at(*base, delta, date)) matches Some(groups) ==>
                i32::MIN <= date <= i32::MAX && groups.valid() && groups.size() > 0 &&
                forall|key: i64| (#[trigger] groups.at(key)) matches Some(counts) ==> counts_ok(counts@),
        decreases work.keys.len() - position,
    {
        let date = work.keys[position];
        proof {
            assert(!original.keys@.take(position as int).contains(date));
            assert(original.data@.contains_key(date));
        }
        let old_groups = match base.get(date) { Some(value) => value, None => Arc::new(Index::new()) };
        let changes = match work.take(date) { Some(value) => value, None => return None };
        let groups = match crate::kernel::finish_customers(&old_groups, changes) { Some(value) => value, None => return None };
        proof { use_type_invariant(&groups); }
        let ghost previous_delta = delta;
        proof {
            assert forall|key: i64, priority: i64| #[trigger] c_index(groups, key, priority) ==
                date_value(*base, date as i32, key, priority) + date_work_value(original, date as i32, key, priority) by {
                assert(c_index(groups, key, priority) == c_index(*old_groups, key, priority) + work_count(changes, key, priority));
                if base.at(date).is_none() { old_groups.lemma_empty(); }
            }
        }
        delta.put(date, if groups.len() == 0 { None } else { Some(Arc::new(groups)) });
        proof {
            assert(original.keys@.take(position as int + 1) =~= original.keys@.take(position as int).push(date));
            assert(original.keys@.take(position as int + 1).to_set() =~= original.keys@.take(position as int).to_set().insert(date));
            assert(work.data@ =~= original.data@.remove_keys(original.keys@.take(position as int + 1).to_set()));
            assert forall|other_date: i64| (#[trigger] overlay_at(*base, delta, other_date)).is_some() implies
                i32::MIN <= other_date <= i32::MAX && overlay_at(*base, delta, other_date).unwrap().valid()
                && overlay_at(*base, delta, other_date).unwrap().size() > 0 &&
                forall|key: i64| (#[trigger] overlay_at(*base, delta, other_date).unwrap().at(key)) matches Some(counts) ==> counts_ok(counts@) by {
                if other_date == date {
                    assert(overlay_at(*base, delta, other_date).unwrap().size() == groups.size());
                } else {
                    assert(overlay_at(*base, delta, other_date) == overlay_at(*base, previous_delta, other_date));
                }
            }
            assert forall|other_date: i32, key: i64, priority: i64| #[trigger] date_overlay(*base, delta, other_date, key, priority) ==
                date_value(*base, other_date, key, priority) +
                    if original.keys@.take(position as int + 1).contains(other_date as i64) { date_work_value(original, other_date, key, priority) } else { 0 } by {
                if other_date as i64 == date {
                    assert(c_index(groups, key, priority) == date_value(*base, other_date, key, priority)
                        + date_work_value(original, other_date, key, priority));
                    if groups.size() == 0 { groups.lemma_empty(); }
                } else {
                    assert(date_overlay(*base, previous_delta, other_date, key, priority) ==
                        date_value(*base, other_date, key, priority) +
                            if original.keys@.take(position as int).contains(other_date as i64) { date_work_value(original, other_date, key, priority) } else { 0 });
                }
            }
        }
        position += 1;
    }
    proof { assert(original.keys@.take(position as int) =~= original.keys@); }
    let dates = match base.with(delta) { Some(value) => value, None => return None };
    proof {
        assert forall|date: i32, key: i64, priority: i64| #[trigger] date_value(dates, date, key, priority) ==
            date_value(*base, date, key, priority) + date_work_value(original, date, key, priority) by {
            assert(date_value(dates, date, key, priority) == date_overlay(*base, delta, date, key, priority));
            if !original.keys@.contains(date as i64) { assert(!original.data@.contains_key(date as i64)); }
        }
        assert forall|date: i64| (#[trigger] dates.at(date)).is_some() implies
            i32::MIN <= date <= i32::MAX && dates.at(date).unwrap().valid() && dates.at(date).unwrap().size() > 0 &&
            forall|key: i64| (#[trigger] dates.at(date).unwrap().at(key)) matches Some(counts) ==> counts_ok(counts@) by {
            assert(dates.at(date) == overlay_at(*base, delta, date));
        }
    }
    Some(dates)
}

pub fn finish(base: &JoinIndex, mut work: JoinWork) -> (result: Option<JoinIndex>)
    requires join_index_ok(*base), join_work_ok(work),
    ensures result matches Some(index) ==> join_index_ok(index),
        result matches Some(index) ==> forall|segment: i64, date: i32, key: i64, priority: i64|
            #[trigger] h_at(index, segment, date, key, priority) ==
                h_at(*base, segment, date, key, priority) + h_work(work, segment, date, key, priority),
{
    let ghost original = work;
    let mut segments = Run::new();
    let mut segment_position = 0;
    while segment_position < work.keys.len()
        invariant segment_position <= work.keys.len(),
            run_ok(segments), join_index_ok(*base), join_work_ok(original),
            work.keys@ == original.keys@, work_directory(work),
            work.data@ == original.data@.remove_keys(original.keys@.take(segment_position as int).to_set()),
            segments.model().dom() == original.keys@.take(segment_position as int).to_set(),
            forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] join_overlay(*base, segments, segment, date, key, priority) ==
                h_at(*base, segment, date, key, priority) +
                    if original.keys@.take(segment_position as int).contains(segment) { h_work(original, segment, date, key, priority) } else { 0 },
            forall|segment: i64| (#[trigger] overlay_at(*base, segments, segment)) matches Some(dates) ==> dates_ok(*dates) && dates.size() > 0,
        decreases work.keys.len() - segment_position,
    {
        let segment = work.keys[segment_position];
        proof {
            assert(!original.keys@.take(segment_position as int).contains(segment));
            assert(original.data@.contains_key(segment));
        }
        let old_dates = match base.get(segment) { Some(value) => value, None => Arc::new(Index::new()) };
        let date_work = match work.take(segment) { Some(value) => value, None => return None };
        let dates = match finish_dates(&old_dates, date_work) { Some(value) => value, None => return None };
        proof { use_type_invariant(&dates); }
        let ghost previous_segments = segments;
        segments.put(segment, if dates.len() == 0 { None } else { Some(Arc::new(dates)) });
        proof {
            assert(original.keys@.take(segment_position as int + 1) =~= original.keys@.take(segment_position as int).push(segment));
            assert(original.keys@.take(segment_position as int + 1).to_set() =~= original.keys@.take(segment_position as int).to_set().insert(segment));
            assert(work.data@ =~= original.data@.remove_keys(original.keys@.take(segment_position as int + 1).to_set()));
            assert forall|other_segment: i64| (#[trigger] overlay_at(*base, segments, other_segment)).is_some() implies
                dates_ok(*overlay_at(*base, segments, other_segment).unwrap()) && overlay_at(*base, segments, other_segment).unwrap().size() > 0 by {
                if other_segment != segment {
                    assert(overlay_at(*base, segments, other_segment) == overlay_at(*base, previous_segments, other_segment));
                }
            }
            assert forall|other_segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] join_overlay(*base, segments, other_segment, date, key, priority) ==
                h_at(*base, other_segment, date, key, priority) +
                    if original.keys@.take(segment_position as int + 1).contains(other_segment) { h_work(original, other_segment, date, key, priority) } else { 0 } by {
                if other_segment == segment {
                    assert(date_value(dates, date, key, priority) == date_value(*old_dates, date, key, priority) + date_work_value(date_work, date, key, priority));
                    if base.at(segment).is_none() { old_dates.lemma_empty(); }
                    if dates.size() == 0 { dates.lemma_empty(); }
                } else {
                    assert(join_overlay(*base, previous_segments, other_segment, date, key, priority) ==
                        h_at(*base, other_segment, date, key, priority) +
                            if original.keys@.take(segment_position as int).contains(other_segment) { h_work(original, other_segment, date, key, priority) } else { 0 });
                }
            }
        }
        segment_position += 1;
    }
    proof { assert(original.keys@.take(segment_position as int) =~= original.keys@); }
    let index = match base.with(segments) { Some(value) => value, None => return None };
    proof {
        assert forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_at(index, segment, date, key, priority) ==
            h_at(*base, segment, date, key, priority) + h_work(original, segment, date, key, priority) by {
            assert(h_at(index, segment, date, key, priority) == join_overlay(*base, segments, segment, date, key, priority));
            if !original.keys@.contains(segment) { assert(!original.data@.contains_key(segment)); }
        }
        assert forall|segment: i64| (#[trigger] index.at(segment)).is_some() implies
            dates_ok(*index.at(segment).unwrap()) && index.at(segment).unwrap().size() > 0 by {
            assert(index.at(segment) == overlay_at(*base, segments, segment));
        }
    }
    Some(index)
}

}
