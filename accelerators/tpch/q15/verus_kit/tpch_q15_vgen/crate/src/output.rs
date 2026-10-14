use vstd::prelude::*;
use vstd::hash_map::HashMapWithView;
use verus_common::sums::*;
use verus_prooflib::ordered::{Key, Cursor};
use crate::model::*;
use crate::values::*;
use crate::values::add;
use crate::gen_spec::{Db, query_result};
use crate::gen_obligations::{params, out_view};
use crate::query_proofs::*;

verus! {
pub type OutputRow = (i64, Vec<u8>, Vec<u8>, Vec<u8>, i128);
pub struct Stream {
    cursor: Cursor<BucketProfile>,
    full: bool,
    lower: i32,
    upper: i32,
}
#[derive(Clone, Copy)]
pub struct Head { pub supplier: i64, pub revenue: i128, pub stream: usize, pub count: Ghost<int> }

pub open spec fn selected_count(profile: BucketProfile, lower: int, upper: int) -> int {
    sum(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.count as int } else { 0int })
}
pub open spec fn selected_revenue(profile: BucketProfile, lower: int, upper: int) -> int {
    sum(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.revenue as int } else { 0int })
}
pub proof fn lemma_selected_zero(profile: BucketProfile, lower: int, upper: int)
    requires forall|position: int| 0 <= position < profile.days.len() ==> (#[trigger] profile.days@[position]).count > 0,
    ensures selected_count(profile, lower, upper) >= 0,
        selected_count(profile, lower, upper) == 0 ==> selected_revenue(profile, lower, upper) == 0,
{
    let count = |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.count as int } else { 0int };
    lemma_sum_nonneg(profile.days@, count);
    if selected_count(profile, lower, upper) == 0 {
        assert forall|position: int| 0 <= position < profile.days.len() implies
            !(lower <= (#[trigger] profile.days@[position]).shipdate as int && (profile.days@[position].shipdate as int) < upper) by {
            if lower <= profile.days@[position].shipdate as int && (profile.days@[position].shipdate as int) < upper {
                lemma_sum_pos(profile.days@, count, position);
            }
        }
        lemma_sum_zero(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.revenue as int } else { 0int });
    }
}
pub proof fn lemma_profile_full(profile: BucketProfile, bin: int, lower: int, upper: int)
    requires profile_shape(profile, bin), lower <= bin * 256, upper >= (bin + 1) * 256,
    ensures selected_count(profile, lower, upper) == profile.count as int,
        selected_revenue(profile, lower, upper) == profile.revenue as int,
{
    assert forall|position: int| 0 <= position < profile.days.len() implies
        lower <= (#[trigger] profile.days@[position]).shipdate as int && (profile.days@[position].shipdate as int) < upper by {
        let date = profile.days@[position].shipdate as int;
        vstd::arithmetic::div_mod::lemma_fundamental_div_mod(date, 256);
        vstd::arithmetic::div_mod::lemma_mod_bound(date, 256);
        assert(date / 256 == bin);
    }
    lemma_sum_ext(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.count as int } else { 0int }, |day: Day| day.count as int);
    lemma_sum_ext(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.revenue as int } else { 0int }, |day: Day| day.revenue as int);
}
pub open spec fn pending_weight(supplier: int, lower: int, upper: int, revenue: bool) -> spec_fn((verus_prooflib::ordered::KeyView, BucketProfile)) -> int {
    |entry: (verus_prooflib::ordered::KeyView, BucketProfile)| if entry.0 == number_key(supplier) {
        if revenue { selected_revenue(entry.1, lower, upper) } else { selected_count(entry.1, lower, upper) }
    } else { 0int }
}
pub closed spec fn stream_sum(stream: Stream, supplier: int, revenue: bool) -> int {
    sum(stream.cursor.remaining(), pending_weight(supplier, stream.lower as int, stream.upper as int, revenue))
}
pub open spec fn head_weight(supplier: int, revenue: bool) -> spec_fn(Head) -> int {
    |head: Head| if head.supplier as int == supplier { if revenue { head.revenue as int } else { head.count@ } } else { 0int }
}
pub open spec fn head_sum(heads: Seq<Head>, supplier: int, revenue: bool) -> int {
    sum(heads, head_weight(supplier, revenue))
}
pub proof fn lemma_pending_first(rows: Seq<(verus_prooflib::ordered::KeyView, BucketProfile)>, supplier: int, lower: int, upper: int, revenue: bool)
    requires rows.len() > 0,
    ensures sum(rows, pending_weight(supplier, lower, upper, revenue)) ==
        pending_weight(supplier, lower, upper, revenue)(rows[0]) + sum(rows.skip(1), pending_weight(supplier, lower, upper, revenue)),
{
    let weight = pending_weight(supplier, lower, upper, revenue);
    assert(rows =~= seq![rows[0]] + rows.skip(1));
    lemma_sum_concat(seq![rows[0]], rows.skip(1), weight);
    lemma_sum_singleton(rows[0], weight);
}
pub fn lower_bound(days: &Vec<Day>, date: i32) -> (result: usize)
    requires forall|left: int, right: int| #![trigger days@[left], days@[right]]
        0 <= left < right < days.len() ==> days@[left].shipdate < days@[right].shipdate,
    ensures result <= days.len(),
        forall|position: int| #[trigger] days@[position].shipdate < date || !(0 <= position < result),
        forall|position: int| #[trigger] days@[position].shipdate >= date || !(result <= position < days.len()),
{
    if days.len() == 0 { return 0; }
    if date <= days[0].shipdate {
        proof {
            assert forall|position: int| 0 <= position < days.len() implies
                (#[trigger] days@[position]).shipdate >= date by {
                if position > 0 { assert(days@[0].shipdate < days@[position].shipdate); }
            }
        }
        return 0;
    }
    let last = days.len() - 1;
    if days[last].shipdate < date {
        proof {
            assert forall|position: int| 0 <= position < days.len() implies
                (#[trigger] days@[position]).shipdate < date by {
                if position < last { assert(days@[position].shipdate < days@[last as int].shipdate); }
            }
        }
        return days.len();
    }
    let mut lower = 0;
    let mut upper = days.len();
    while lower < upper
        invariant lower <= upper <= days.len(),
            forall|left: int, right: int| #![trigger days@[left], days@[right]]
                0 <= left < right < days.len() ==> days@[left].shipdate < days@[right].shipdate,
            forall|position: int| #[trigger] days@[position].shipdate < date || !(0 <= position < lower),
            forall|position: int| #[trigger] days@[position].shipdate >= date || !(upper <= position < days.len()),
        decreases upper - lower,
    {
        let middle = lower + (upper - lower) / 2;
        if days[middle].shipdate < date {
            proof {
                assert forall|position: int| 0 <= position < middle + 1 implies
                    (#[trigger] days@[position]).shipdate < date by {
                    if position < lower { assert(days@[position].shipdate < date); }
                    else if position < middle { assert(days@[position].shipdate < days@[middle as int].shipdate); }
                }
            }
            lower = middle + 1;
        } else {
            proof {
                assert forall|position: int| middle <= position < days.len() implies
                    (#[trigger] days@[position]).shipdate >= date by {
                    if position >= upper { assert(days@[position].shipdate >= date); }
                    else if middle < position { assert(days@[middle as int].shipdate < days@[position].shipdate); }
                }
            }
            upper = middle;
        }
    }
    lower
}
pub fn partial(profile: &BucketProfile, lower: i32, upper: i32) -> (result: Option<(bool, i128, Ghost<int>)>)
    requires lower < upper,
        forall|position: int| 0 <= position < profile.days.len() ==> (#[trigger] profile.days@[position]).count > 0,
        forall|left: int, right: int| #![trigger profile.days@[left], profile.days@[right]]
            0 <= left < right < profile.days.len() ==> profile.days@[left].shipdate < profile.days@[right].shipdate,
    ensures result matches Some(pair) ==> pair.2@ == selected_count(*profile, lower as int, upper as int)
        && (pair.0 <==> pair.2@ > 0)
        && pair.1 == selected_revenue(*profile, lower as int, upper as int),
{
    let start = lower_bound(&profile.days, lower);
    let end = lower_bound(&profile.days, upper);
    proof {
        if end < start {
            assert(profile.days@[end as int].shipdate < lower);
            assert(profile.days@[end as int].shipdate >= upper);
            assert(false);
        }
        let keep = |day: Day| lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int;
        assert forall|position: int| 0 <= position < profile.days.len() implies
            keep(#[trigger] profile.days@[position]) == (start <= position < end) by {
            if position < start { assert(profile.days@[position].shipdate < lower); }
            else { assert(profile.days@[position].shipdate >= lower); }
            if position < end { assert(profile.days@[position].shipdate < upper); }
            else { assert(profile.days@[position].shipdate >= upper); }
        }
    }
    let mut position = start;
    let mut count = Ghost(0int);
    let mut revenue = 0i128;
    while position < end
        invariant start <= position <= end <= profile.days.len(),
            forall|offset: int| 0 <= offset < profile.days.len() ==>
                (lower as int <= (#[trigger] profile.days@[offset]).shipdate as int && (profile.days@[offset].shipdate as int) < upper as int)
                    == (start <= offset < end),
            count@ == sum(profile.days@.subrange(start as int, position as int), |day: Day| day.count as int),
            revenue == sum(profile.days@.subrange(start as int, position as int), |day: Day| day.revenue as int),
        decreases end - position,
    {
        proof {
            let before = profile.days@.subrange(start as int, position as int);
            let after = profile.days@.subrange(start as int, position as int + 1);
            assert(after =~= before.push(profile.days@[position as int]));
            lemma_sum_push(before, profile.days@[position as int], |day: Day| day.count as int);
            lemma_sum_push(before, profile.days@[position as int], |day: Day| day.revenue as int);
        }
        count = Ghost(count@ + profile.days@[position as int].count as int);
        revenue = match add(revenue, profile.days[position].revenue as i128) { Some(value) => value, None => return None };
        position += 1;
    }
    proof {
        let keep = |day: Day| lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int;
        assert forall|position: int| 0 <= position < profile.days.len() implies
            keep(#[trigger] profile.days@[position]) == (start <= position < end) by {
            if position < start { assert(profile.days@[position].shipdate < lower); }
            else { assert(profile.days@[position].shipdate >= lower); }
            if position < end { assert(profile.days@[position].shipdate < upper); }
            else { assert(profile.days@[position].shipdate >= upper); }
        }
        let counts = |day: Day| day.count as int;
        let revenues = |day: Day| day.revenue as int;
        lemma_window_sum(profile.days@, start as int, end as int, keep, counts);
        lemma_window_sum(profile.days@, start as int, end as int, keep, revenues);
        lemma_sum_ext_all(profile.days@, |day: Day| if keep(day) { counts(day) } else { 0int },
            |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.count as int } else { 0int });
        lemma_sum_ext_all(profile.days@, |day: Day| if keep(day) { revenues(day) } else { 0int },
            |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.revenue as int } else { 0int });
        assert(count@ == selected_count(*profile, lower as int, upper as int));
        assert(revenue == selected_revenue(*profile, lower as int, upper as int));
        let selected = profile.days@.subrange(start as int, end as int);
        assert forall|offset: int| 0 <= offset < selected.len() implies
            (#[trigger] selected[offset]).count > 0 by {
            assert(selected[offset] == profile.days@[start as int + offset]);
        }
        if start < end {
            lemma_sum_nonneg(selected, counts);
            lemma_sum_pos(selected, counts, 0);
        } else {
            assert(selected =~= Seq::<Day>::empty());
            lemma_sum_empty(selected, counts);
        }
    }
    Some((start < end, revenue, count))
}
pub proof fn lemma_window_sum(days: Seq<Day>, start: int, end: int, keep: spec_fn(Day) -> bool, weight: spec_fn(Day) -> int)
    requires 0 <= start <= end <= days.len(),
        forall|position: int| 0 <= position < days.len() ==> keep(#[trigger] days[position]) == (start <= position < end),
    ensures sum(days.subrange(start, end), weight) == sum(days, |day: Day| if keep(day) { weight(day) } else { 0int }),
    decreases days.len(),
{
    let filtered = |day: Day| if keep(day) { weight(day) } else { 0int };
    if days.len() > 0 {
        let prefix = days.drop_last();
        if end < days.len() {
            lemma_window_sum(prefix, start, end, keep, weight);
            assert(days.subrange(start, end) =~= prefix.subrange(start, end));
            assert(!keep(days.last()));
        } else if start < end {
            lemma_window_sum(prefix, start, end - 1, keep, weight);
            assert(days.subrange(start, end) =~= prefix.subrange(start, end - 1).push(days.last()));
            assert(keep(days.last()));
            lemma_sum_push(prefix.subrange(start, end - 1), days.last(), weight);
        } else {
            assert(days.subrange(start, end).len() == 0);
            lemma_sum_zero(days, filtered);
        }
    }
}
pub closed spec fn stream_valid(stream: Stream) -> bool {
    stream.lower < stream.upper &&
    (forall|position: int| 0 <= position < stream.cursor.remaining().len() ==> {
        let profile = (#[trigger] stream.cursor.remaining()[position]).1;
        stream.cursor.remaining()[position].0.1 == Seq::<u8>::empty() &&
        (forall|offset: int| 0 <= offset < profile.days.len() ==> (#[trigger] profile.days@[offset]).count > 0) &&
        (stream.full ==> profile.count as int == selected_count(profile, stream.lower as int, stream.upper as int) &&
            profile.revenue as int == selected_revenue(profile, stream.lower as int, stream.upper as int)) &&
        (forall|left: int, right: int| #![trigger profile.days@[left], profile.days@[right]]
            0 <= left < right < profile.days.len() ==> profile.days@[left].shipdate < profile.days@[right].shipdate)
    }) &&
    (forall|left: int, right: int| #![trigger stream.cursor.remaining()[left], stream.cursor.remaining()[right]]
        0 <= left < right < stream.cursor.remaining().len() ==>
            stream.cursor.remaining()[left].0.0 < stream.cursor.remaining()[right].0.0)
}
fn next(stream: &mut Stream, id: usize) -> (result: Option<Option<Head>>)
    requires stream_valid(*old(stream)),
    ensures stream_valid(*final(stream)),
        result is Some ==> final(stream).lower == old(stream).lower && final(stream).upper == old(stream).upper && final(stream).full == old(stream).full,
        result matches Some(Some(head)) ==> head.count@ > 0 && forall|supplier: int, revenue: bool| #[trigger] stream_sum(*old(stream), supplier, revenue) ==
            stream_sum(*final(stream), supplier, revenue) + head_weight(supplier, revenue)(head),
        result matches Some(None) ==> forall|supplier: int, revenue: bool| #[trigger] stream_sum(*old(stream), supplier, revenue) == 0,
        result matches Some(Some(head)) ==> head.stream == id &&
            (exists|position: int| 0 <= position < old(stream).cursor.remaining().len() &&
                (#[trigger] old(stream).cursor.remaining()[position]).0.0 == head.supplier as int),
        result matches Some(Some(head)) ==> forall|position: int| 0 <= position < final(stream).cursor.remaining().len() ==>
            (#[trigger] final(stream).cursor.remaining()[position]).0.0 > head.supplier as int,
        result matches Some(None) ==> final(stream).cursor.remaining().len() == 0,
        result is Some ==> forall|position: int| 0 <= position < final(stream).cursor.remaining().len() ==>
            #[trigger] old(stream).cursor.remaining().contains(final(stream).cursor.remaining()[position]),
        result is Some ==> final(stream).cursor.remaining().len() <= old(stream).cursor.remaining().len(),
        result matches Some(Some(head)) ==> final(stream).cursor.remaining().len() < old(stream).cursor.remaining().len(),
{
    let ghost initial = stream.cursor.remaining();
    let ghost initial_stream = *stream;
    let mut processed = Ghost(0int);
    loop
        invariant stream_valid(*stream),
            initial == old(stream).cursor.remaining(),
            initial_stream == *old(stream), stream.lower == initial_stream.lower, stream.upper == initial_stream.upper, stream.full == initial_stream.full,
            forall|supplier: int, revenue: bool| #[trigger] stream_sum(*stream, supplier, revenue) == stream_sum(initial_stream, supplier, revenue),
            0 <= processed@ <= initial.len(), stream.cursor.remaining() == initial.skip(processed@),
        decreases stream.cursor.work(),
    {
        let ghost before = stream.cursor.remaining();
        proof {
            assert forall|supplier: int, revenue: bool| #[trigger] stream_sum(initial_stream, supplier, revenue) ==
                sum(before, pending_weight(supplier, stream.lower as int, stream.upper as int, revenue)) by {
                assert(stream_sum(*stream, supplier, revenue) == stream_sum(initial_stream, supplier, revenue));
            }
        }
        let (key, profile) = match stream.cursor.next() {
            Some(value) => value, None => {
                proof {
                    assert(before.len() == 0);
                    assert forall|supplier: int, revenue: bool| #[trigger] stream_sum(initial_stream, supplier, revenue) == 0 by {
                        lemma_sum_empty(before, pending_weight(supplier, stream.lower as int, stream.upper as int, revenue));
                    }
                }
                return Some(None);
            },
        };
        proof {
            assert(*profile == before[0].1);
            assert(key.view() == before[0].0);
            assert(key.text@ == Seq::<u8>::empty());
            assert forall|left: int, right: int| #![trigger stream.cursor.remaining()[left], stream.cursor.remaining()[right]]
                0 <= left < right < stream.cursor.remaining().len() implies
                    stream.cursor.remaining()[left].0.0 < stream.cursor.remaining()[right].0.0 by {
                assert(stream.cursor.remaining()[left] == before[left + 1]);
                assert(stream.cursor.remaining()[right] == before[right + 1]);
            }
            assert forall|position: int| 0 <= position < stream.cursor.remaining().len() implies {
                let profile = (#[trigger] stream.cursor.remaining()[position]).1;
                stream.cursor.remaining()[position].0.1 == Seq::<u8>::empty() &&
                (forall|offset: int| 0 <= offset < profile.days.len() ==> (#[trigger] profile.days@[offset]).count > 0) &&
                (stream.full ==> profile.count as int == selected_count(profile, stream.lower as int, stream.upper as int) &&
                    profile.revenue as int == selected_revenue(profile, stream.lower as int, stream.upper as int)) &&
                (forall|left: int, right: int| #![trigger profile.days@[left], profile.days@[right]]
                    0 <= left < right < profile.days.len() ==> profile.days@[left].shipdate < profile.days@[right].shipdate
                )
            } by { assert(stream.cursor.remaining()[position] == before[position + 1]); }
            assert forall|position: int| 0 <= position < stream.cursor.remaining().len() implies
                (#[trigger] stream.cursor.remaining()[position]).0.0 > key.number as int by {
                assert(before[0].0.0 < before[position + 1].0.0);
            }
            assert(initial[processed@].0.0 == key.number as int);
            assert forall|position: int| 0 <= position < stream.cursor.remaining().len() implies
                #[trigger] initial.contains(stream.cursor.remaining()[position]) by {
                assert(stream.cursor.remaining()[position] == initial[processed@ + 1 + position]);
            }
        }
        let pair = if stream.full { (profile.count > 0, profile.revenue, Ghost(profile.count as int)) }
        else {
            match partial(&*profile, stream.lower, stream.upper) { Some(value) => value, None => return None }
        };
        proof {
            let ghost selected_profile = *profile;
            lemma_selected_zero(selected_profile, stream.lower as int, stream.upper as int);
            assert(pair.2@ == selected_count(*profile, stream.lower as int, stream.upper as int));
            assert(pair.1 == selected_revenue(*profile, stream.lower as int, stream.upper as int));
            assert forall|supplier: int, revenue: bool| #[trigger] stream_sum(initial_stream, supplier, revenue) == stream_sum(*stream, supplier, revenue) +
                if supplier == key.number as int { if revenue { pair.1 as int } else { pair.2@ } } else { 0int } by {
                lemma_pending_first(before, supplier, stream.lower as int, stream.upper as int, revenue);
                assert(stream_sum(initial_stream, supplier, revenue) == sum(before, pending_weight(supplier, stream.lower as int, stream.upper as int, revenue)));
                assert(stream_sum(*stream, supplier, revenue) == sum(before.skip(1), pending_weight(supplier, stream.lower as int, stream.upper as int, revenue)));
                assert(pending_weight(supplier, stream.lower as int, stream.upper as int, revenue)(before[0]) ==
                    if supplier == key.number as int { if revenue { pair.1 as int } else { pair.2@ } } else { 0int });
            }
        }
        if pair.0 {
            proof {
                assert(0 <= processed@ < initial.len());
                assert(initial[processed@].0.0 == key.number as int);
                assert(exists|position: int| 0 <= position < initial.len() && (#[trigger] initial[position]).0.0 == key.number as int);
            }
            return Some(Some(Head { supplier: key.number, revenue: pair.1, stream: id, count: pair.2 }));
        }
        processed = Ghost(processed@ + 1);
    }
}
pub open spec fn heap_valid(heap: Seq<Head>) -> bool {
    forall|position: int| #[trigger] heap[position].supplier >= heap[(position - 1) / 2].supplier
        || !(0 < position < heap.len())
}
pub open spec fn heap_up(heap: Seq<Head>, hole: int) -> bool {
    (forall|position: int| 0 < position < heap.len() && position != hole ==>
        (#[trigger] heap[position]).supplier >= heap[(position - 1) / 2].supplier) &&
    (forall|position: int| 0 < position < heap.len() && (position - 1) / 2 == hole && hole > 0 ==>
        (#[trigger] heap[position]).supplier >= heap[(hole - 1) / 2].supplier)
}
pub proof fn lemma_head_swap(rows: Seq<Head>, first: int, second: int)
    requires 0 <= first < rows.len(), 0 <= second < rows.len(), first != second,
    ensures rows.update(first, rows[second]).update(second, rows[first]).to_multiset() == rows.to_multiset(),
{
    broadcast use vstd::multiset::group_multiset_axioms;
    let middle = rows.update(first, rows[second]);
    let after = middle.update(second, rows[first]);
    assert forall|value: Head| #[trigger] after.to_multiset().count(value) == rows.to_multiset().count(value) by {
        lemma_count_as_sum(rows, value);
        lemma_count_as_sum(after, value);
        lemma_sum_update(rows, first, rows[second], eqv(value));
        lemma_sum_update(middle, second, rows[first], eqv(value));
    }
}
pub proof fn lemma_sift_up(rows: Seq<Head>, position: int)
    requires 0 < position < rows.len(), heap_up(rows, position), rows[position].supplier < rows[(position - 1) / 2].supplier,
    ensures heap_up(rows.update(position, rows[(position - 1) / 2]).update((position - 1) / 2, rows[position]), (position - 1) / 2),
{
    let parent = (position - 1) / 2;
    let after = rows.update(position, rows[parent]).update(parent, rows[position]);
    assert(0 <= parent < position);
    assert forall|index: int| 0 < index < after.len() && index != parent implies
        (#[trigger] after[index]).supplier >= after[(index - 1) / 2].supplier by {
        let ancestor = (index - 1) / 2;
        if index == position { assert(after[ancestor] == rows[position]); }
        else if ancestor == position { assert(rows[index].supplier >= rows[parent].supplier); }
        else if ancestor == parent { assert(rows[index].supplier >= rows[parent].supplier); }
        else { assert(after[index] == rows[index]); assert(after[ancestor] == rows[ancestor]); }
    }
    assert forall|index: int| 0 < index < after.len() && (index - 1) / 2 == parent && parent > 0 implies
        (#[trigger] after[index]).supplier >= after[(parent - 1) / 2].supplier by {
        let grandparent = (parent - 1) / 2;
        assert(0 <= grandparent < parent);
        assert(rows[parent].supplier >= rows[grandparent].supplier);
        if index == position { assert(after[index] == rows[parent]); }
        else { assert(rows[index].supplier >= rows[parent].supplier); }
    }
}
#[verifier::rlimit(20)]
pub fn push(heap: &mut Vec<Head>, value: Head)
    requires heap_valid(old(heap)@), old(heap).len() < usize::MAX,
    ensures heap_valid(final(heap)@), final(heap)@.to_multiset() == old(heap)@.to_multiset().insert(value),
        final(heap).len() == old(heap).len() + 1,
{
    let ghost before = heap@;
    let mut position = heap.len();
    let mut finished = false;
    heap.push(value);
    proof {
        broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
        assert(heap@.to_multiset() == before.to_multiset().insert(value));
        assert(heap_up(heap@, position as int));
    }
    while position > 0 && !finished
        invariant position < heap.len(), heap.len() == before.len() + 1, heap@[position as int] == value,
            heap_up(heap@, position as int), heap@.to_multiset() == before.to_multiset().insert(value),
            finished ==> heap_valid(heap@),
        decreases if finished { 0int } else { position as int + 1 },
    {
        let parent = (position - 1) / 2;
        if heap[parent].supplier <= value.supplier {
            proof { lemma_up_done(heap@, position as int); }
            finished = true;
        } else {
            let moved = heap[parent];
            proof {
                lemma_head_swap(heap@, position as int, parent as int);
                lemma_sift_up(heap@, position as int);
            }
            heap.set(position, moved);
            heap.set(parent, value);
            position = parent;
        }
    }
    proof {
        if position == 0 { lemma_up_done(heap@, position as int); }
        assert(heap_valid(heap@));
        assert(heap@.update(position as int, value) =~= heap@);
    }
    heap.set(position, value);
}
pub proof fn lemma_up_done(rows: Seq<Head>, position: int)
    requires 0 <= position < rows.len(), heap_up(rows, position), position == 0 || rows[(position - 1) / 2].supplier <= rows[position].supplier,
    ensures heap_valid(rows),
{
    assert forall|index: int| 0 < index < rows.len() implies (#[trigger] rows[index]).supplier >= rows[(index - 1) / 2].supplier by {
        if index == position { assert(position > 0); }
    }
}
pub proof fn lemma_heap_min(rows: Seq<Head>, position: int)
    requires heap_valid(rows), 0 <= position < rows.len(),
    ensures rows[position].supplier >= rows[0].supplier,
    decreases position,
{
    if position > 0 {
        let parent = (position - 1) / 2;
        assert(0 <= parent < position);
        lemma_heap_min(rows, parent);
        assert(rows[position].supplier >= rows[parent].supplier);
    }
}
pub open spec fn heap_down(rows: Seq<Head>, hole: int) -> bool {
    (forall|position: int| 0 < position < rows.len() && (position - 1) / 2 != hole ==>
        (#[trigger] rows[position]).supplier >= rows[(position - 1) / 2].supplier) &&
    (forall|position: int| 0 < position < rows.len() && (position - 1) / 2 == hole && hole > 0 ==>
        (#[trigger] rows[position]).supplier >= rows[(hole - 1) / 2].supplier)
}
pub proof fn lemma_down_step(rows: Seq<Head>, position: int, child: int)
    requires 0 <= position < child < rows.len(), (child - 1) / 2 == position,
        heap_down(rows, position), rows[child].supplier < rows[position].supplier,
        forall|index: int| 0 < index < rows.len() && (index - 1) / 2 == position ==>
            (#[trigger] rows[index]).supplier >= rows[child].supplier,
    ensures heap_down(rows.update(position, rows[child]).update(child, rows[position]), child),
{
    let after = rows.update(position, rows[child]).update(child, rows[position]);
    assert forall|index: int| 0 < index < after.len() && (index - 1) / 2 != child implies
        (#[trigger] after[index]).supplier >= after[(index - 1) / 2].supplier by {
        let parent = (index - 1) / 2;
        if index == child { assert(after[parent] == rows[child]); }
        else if index == position {
            assert(position > 0);
            assert(rows[child].supplier >= rows[(position - 1) / 2].supplier);
        } else if parent == position { assert(rows[index].supplier >= rows[child].supplier); }
        else { assert(after[index] == rows[index]); assert(after[parent] == rows[parent]); }
    }
    assert forall|index: int| 0 < index < after.len() && (index - 1) / 2 == child implies
        (#[trigger] after[index]).supplier >= after[(child - 1) / 2].supplier by {
        assert(rows[index].supplier >= rows[child].supplier);
    }
}
pub proof fn lemma_down_done(rows: Seq<Head>, position: int)
    requires heap_down(rows, position),
        forall|index: int| 0 < index < rows.len() && (index - 1) / 2 == position ==>
            (#[trigger] rows[index]).supplier >= rows[position].supplier,
    ensures heap_valid(rows),
{
}
#[verifier::rlimit(20)]
#[verifier::spinoff_prover]
pub fn pop(heap: &mut Vec<Head>) -> (result: Option<Head>)
    requires heap_valid(old(heap)@),
    ensures result matches Some(value) ==> final(heap)@.to_multiset() == old(heap)@.to_multiset().remove(value),
        heap_valid(final(heap)@),
        result matches Some(value) ==> forall|position: int| #[trigger] old(heap)@[position].supplier >= value.supplier
            || !(0 <= position < old(heap).len()),
        result is Some <==> old(heap).len() > 0,
        result is Some ==> final(heap).len() + 1 == old(heap).len(),
        result matches Some(value) ==> value == old(heap)@[0],
{
    let ghost original = heap@;
    proof { broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms; }
    if heap.len() == 0 { return None; }
    let first = heap[0];
    proof {
        assert forall|position: int| 0 <= position < original.len() implies
            (#[trigger] original[position]).supplier >= first.supplier by { lemma_heap_min(original, position); }
    }
    let last = match heap.pop() { Some(value) => value, None => return None };
    proof {
        broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
        assert(original =~= heap@.push(last));
        assert(original.to_multiset() == heap@.to_multiset().insert(last));
    }
    if heap.len() == 0 {
        proof {
            broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
            assert(first == last);
            assert(heap@.to_multiset() == original.to_multiset().remove(first));
        }
        return Some(first);
    }
    let ghost prefix = heap@;
    heap.set(0, last);
    proof {
        broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
        assert(prefix =~= seq![first] + prefix.skip(1));
        assert(heap@ =~= seq![last] + prefix.skip(1));
        assert(heap@.to_multiset() == original.to_multiset().remove(first));
        assert(heap_down(heap@, 0));
    }
    let mut position = 0;
    let mut finished = false;
    while position < heap.len() / 2 && !finished
        invariant position < heap.len(), heap.len() + 1 == original.len(),
            heap@[position as int] == last, heap_down(heap@, position as int),
            heap@.to_multiset() == original.to_multiset().remove(first),
            finished ==> heap_valid(heap@),
        decreases if finished { 0int } else { heap.len() as int - position as int },
    {
        let mut child = position * 2 + 1;
        if child + 1 < heap.len() && heap[child + 1].supplier < heap[child].supplier { child += 1; }
        proof {
            assert forall|index: int| 0 < index < heap.len() && (index - 1) / 2 == position implies
                (#[trigger] heap@[index]).supplier >= heap@[child as int].supplier by {
                assert(index == 2 * position + 1 || index == 2 * position + 2);
            }
        }
        if last.supplier <= heap[child].supplier {
            proof { lemma_down_done(heap@, position as int); }
            finished = true;
        } else {
            let moved = heap[child];
            proof { lemma_down_step(heap@, position as int, child as int); lemma_head_swap(heap@, position as int, child as int); }
            heap.set(position, moved);
            heap.set(child, last);
            position = child;
        }
    }
    proof {
        if !finished {
            assert forall|index: int| 0 < index < heap.len() && (index - 1) / 2 == position implies
                (#[trigger] heap@[index]).supplier >= heap@[position as int].supplier by { assert(false); }
            lemma_down_done(heap@, position as int);
        }
        assert(heap@.update(position as int, last) =~= heap@);
    }
    heap.set(position, last);
    Some(first)
}
pub closed spec fn remaining(streams: HashMapWithView<usize, Stream>, ids: Seq<usize>, heap: Seq<Head>) -> int {
    heap.len() + sum(ids, stream_sizes(streams))
}
pub closed spec fn stream_sizes(streams: HashMapWithView<usize, Stream>) -> spec_fn(usize) -> int {
    |id: usize| if streams@.contains_key(id) { streams@[id].cursor.remaining().len() as int } else { 0int }
}
pub closed spec fn frontier_weight(streams: HashMapWithView<usize, Stream>, supplier: int, revenue: bool) -> spec_fn(usize) -> int {
    |id: usize| if streams@.contains_key(id) { stream_sum(streams@[id], supplier, revenue) } else { 0int }
}
pub closed spec fn frontier_sum(streams: HashMapWithView<usize, Stream>, ids: Seq<usize>, heap: Seq<Head>, supplier: int, revenue: bool) -> int {
    head_sum(heap, supplier, revenue) + sum(ids, frontier_weight(streams, supplier, revenue))
}
proof fn lemma_stream_sums_change(before: HashMapWithView<usize, Stream>, after: HashMapWithView<usize, Stream>, ids: Seq<usize>, id: usize, supplier: int, revenue: bool)
    requires id < ids.len(), ids[id as int] == id, before@.contains_key(id), after@.contains_key(id),
        forall|position: int| 0 <= position < ids.len() ==> (#[trigger] ids[position]) as int == position,
        forall|other: usize| #[trigger] after@.contains_key(other) == before@.contains_key(other),
        forall|other: usize| other != id && before@.contains_key(other) ==> #[trigger] after@[other] == before@[other],
    ensures sum(ids, frontier_weight(before, supplier, revenue)) == sum(ids, frontier_weight(after, supplier, revenue)) +
        stream_sum(before@[id], supplier, revenue) - stream_sum(after@[id], supplier, revenue),
{
    let old_values = frontier_weight(before, supplier, revenue);
    let new_values = frontier_weight(after, supplier, revenue);
    let difference = |other: usize| old_values(other) - new_values(other);
    assert forall|position: int| 0 <= position < ids.len() && position != id as int implies difference(#[trigger] ids[position]) == 0 by {
        assert(ids[position] != id);
    }
    lemma_sum_single_match(ids, difference, id as int);
    lemma_sum_sub(ids, old_values, new_values, difference);
}
proof fn lemma_head_remove_sum(before: Seq<Head>, after: Seq<Head>, head: Head, supplier: int, revenue: bool)
    requires after.to_multiset() == before.to_multiset().remove(head), before.contains(head),
    ensures head_sum(before, supplier, revenue) == head_sum(after, supplier, revenue) + head_weight(supplier, revenue)(head),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    assert(after.push(head).to_multiset() == before.to_multiset());
    lemma_sum_perm(before, after.push(head), head_weight(supplier, revenue));
    lemma_sum_push(after, head, head_weight(supplier, revenue));
}
proof fn lemma_head_insert_sum(before: Seq<Head>, after: Seq<Head>, head: Head, supplier: int, revenue: bool)
    requires after.to_multiset() == before.to_multiset().insert(head),
    ensures head_sum(after, supplier, revenue) == head_sum(before, supplier, revenue) + head_weight(supplier, revenue)(head),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    assert(before.push(head).to_multiset() == after.to_multiset());
    lemma_sum_perm(after, before.push(head), head_weight(supplier, revenue));
    lemma_sum_push(before, head, head_weight(supplier, revenue));
}
proof fn lemma_remaining_step(before: HashMapWithView<usize, Stream>, after: HashMapWithView<usize, Stream>, ids: Seq<usize>, id: usize,
    before_heap: Seq<Head>, after_heap: Seq<Head>)
    requires id < ids.len(), ids[id as int] == id,
        forall|position: int| 0 <= position < ids.len() ==> (#[trigger] ids[position]) as int == position,
        before@.contains_key(id), after@.contains_key(id),
        forall|other: usize| #[trigger] after@.contains_key(other) == before@.contains_key(other),
        forall|other: usize| other != id && before@.contains_key(other) ==> #[trigger] after@[other] == before@[other],
        after@[id].cursor.remaining().len() <= before@[id].cursor.remaining().len(),
        after_heap.len() <= before_heap.len(),
        after_heap.len() == before_heap.len() ==> after@[id].cursor.remaining().len() < before@[id].cursor.remaining().len(),
    ensures remaining(after, ids, after_heap) < remaining(before, ids, before_heap),
        remaining(after, ids, after_heap) >= 0,
{
    let old_sizes = stream_sizes(before);
    let new_sizes = stream_sizes(after);
    let difference = |other: usize| old_sizes(other) - new_sizes(other);
    assert forall|position: int| 0 <= position < ids.len() && position != id as int implies difference(#[trigger] ids[position]) == 0 by {
        assert(ids[position] != id);
    }
    lemma_sum_single_match(ids, difference, id as int);
    lemma_sum_sub(ids, old_sizes, new_sizes, difference);
    lemma_sum_nonneg(ids, new_sizes);
}
pub open spec fn maximum_matches(totals: Seq<(i64, i128)>, maximum: Option<i128>, winners: Seq<i64>) -> bool {
    match maximum {
        None => totals.len() == 0 && winners.len() == 0,
        Some(value) => (
            totals.len() > 0 &&
            (forall|position: int| 0 <= position < totals.len() ==> (#[trigger] totals[position]).1 <= value) &&
            (exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value) &&
            winners == winning_keys(totals, value)),
    }
}
pub open spec fn revenue_equal(value: i128) -> spec_fn((i64, i128)) -> bool {
    |pair: (i64, i128)| pair.1 == value
}
pub open spec fn winning_rows(totals: Seq<(i64, i128)>, value: i128) -> Seq<(i64, i128)> {
    totals.filter(revenue_equal(value))
}
pub open spec fn winning_keys(totals: Seq<(i64, i128)>, value: i128) -> Seq<i64> {
    winning_rows(totals, value).map_values(|pair: (i64, i128)| pair.0)
}
pub closed spec fn streams_valid(streams: HashMapWithView<usize, Stream>) -> bool {
    forall|id: usize| #[trigger] streams@.contains_key(id) ==> stream_valid(streams@[id])
}
pub closed spec fn heads_ready(streams: HashMapWithView<usize, Stream>, heap: Seq<Head>) -> bool {
    forall|position: int| 0 <= position < heap.len() ==> {
        let head = #[trigger] heap[position];
        streams@.contains_key(head.stream) &&
        (forall|offset: int| 0 <= offset < streams@[head.stream].cursor.remaining().len() ==>
            (#[trigger] streams@[head.stream].cursor.remaining()[offset]).0.0 > head.supplier as int)
    }
}
pub open spec fn positive_heads(heap: Seq<Head>) -> bool {
    forall|position: int| 0 <= position < heap.len() ==> (#[trigger] heap[position]).count@ > 0
}
pub closed spec fn streams_scheduled(streams: HashMapWithView<usize, Stream>, heap: Seq<Head>) -> bool {
    forall|id: usize| #[trigger] streams@.contains_key(id) && streams@[id].cursor.remaining().len() > 0 ==>
        exists|position: int| 0 <= position < heap.len() && (#[trigger] heap[position]).stream == id
}
proof fn lemma_scheduled_step(before: HashMapWithView<usize, Stream>, after: HashMapWithView<usize, Stream>, before_heap: Seq<Head>, after_heap: Seq<Head>, removed: Head, added: Option<Head>)
    requires streams_scheduled(before, before_heap),
        forall|id: usize| #[trigger] after@.contains_key(id) == before@.contains_key(id),
        forall|id: usize| id != removed.stream && before@.contains_key(id) ==> #[trigger] after@[id] == before@[id],
        after_heap.to_multiset() == match added { Some(head) => before_heap.to_multiset().remove(removed).insert(head), None => before_heap.to_multiset().remove(removed) },
        added is None ==> after@[removed.stream].cursor.remaining().len() == 0,
        added matches Some(head) ==> head.stream == removed.stream,
    ensures streams_scheduled(after, after_heap),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    assert forall|id: usize| #[trigger] after@.contains_key(id) && after@[id].cursor.remaining().len() > 0 implies
        exists|position: int| 0 <= position < after_heap.len() && (#[trigger] after_heap[position]).stream == id by {
        if id == removed.stream {
            assert(added is Some);
            let value = added.unwrap();
            assert(after_heap.to_multiset().contains(value));
            assert(after_heap.contains(value));
            let position = choose|position: int| 0 <= position < after_heap.len() && (#[trigger] after_heap[position]) == value;
        } else {
            let position = choose|position: int| 0 <= position < before_heap.len() && (#[trigger] before_heap[position]).stream == id;
            let value = before_heap[position];
            assert(value != removed);
            assert(before_heap.contains(value));
            assert(before_heap.to_multiset().count(value) > 0);
            assert(after_heap.to_multiset().count(value) > 0);
            assert(after_heap.contains(value));
            let after_position = choose|position: int| 0 <= position < after_heap.len() && (#[trigger] after_heap[position]) == value;
        }
    }
}
proof fn lemma_frontier_zero(streams: HashMapWithView<usize, Stream>, ids: Seq<usize>, heap: Seq<Head>, supplier: int, revenue: bool)
    requires streams_scheduled(streams, heap), heads_ready(streams, heap),
        forall|position: int| 0 <= position < heap.len() ==> (#[trigger] heap[position]).supplier as int > supplier,
    ensures frontier_sum(streams, ids, heap, supplier, revenue) == 0,
{
    lemma_sum_zero(heap, head_weight(supplier, revenue));
    let weight = frontier_weight(streams, supplier, revenue);
    assert forall|position: int| 0 <= position < ids.len() implies weight(#[trigger] ids[position]) == 0 by {
        let id = ids[position];
        if streams@.contains_key(id) {
            let stream = streams@[id];
            if stream.cursor.remaining().len() > 0 {
                let head_position = choose|position: int| 0 <= position < heap.len() && (#[trigger] heap[position]).stream == id;
                assert(heap[head_position].supplier as int > supplier);
                assert forall|offset: int| 0 <= offset < stream.cursor.remaining().len() implies
                    pending_weight(supplier, stream.lower as int, stream.upper as int, revenue)(#[trigger] stream.cursor.remaining()[offset]) == 0 by {
                    assert(stream.cursor.remaining()[offset].0.0 > heap[head_position].supplier as int);
                }
            }
            lemma_sum_zero(stream.cursor.remaining(), pending_weight(supplier, stream.lower as int, stream.upper as int, revenue));
        }
    }
    lemma_sum_zero(ids, weight);
}
proof fn lemma_head_empty_key(heads: Seq<Head>, supplier: int)
    requires positive_heads(heads), head_sum(heads, supplier, false) == 0,
    ensures head_sum(heads, supplier, true) == 0,
{
    let count = head_weight(supplier, false);
    assert forall|position: int| 0 <= position < heads.len() implies (#[trigger] heads[position]).supplier as int != supplier by {
        if heads[position].supplier as int == supplier { lemma_sum_pos(heads, count, position); }
    }
    lemma_sum_zero(heads, head_weight(supplier, true));
}
proof fn lemma_head_group(heads: Seq<Head>, supplier: i64)
    requires positive_heads(heads), heads.len() > 0,
        forall|position: int| 0 <= position < heads.len() ==> (#[trigger] heads[position]).supplier == supplier,
    ensures head_sum(heads, supplier as int, false) > 0,
        head_sum(heads, supplier as int, true) == sum(heads, |head: Head| head.revenue as int),
        forall|query: int, revenue: bool| #[trigger] head_sum(heads, query, revenue) ==
            if query == supplier as int { head_sum(heads, supplier as int, revenue) } else { 0int },
{
    lemma_sum_pos(heads, head_weight(supplier as int, false), 0);
    lemma_sum_ext(heads, head_weight(supplier as int, true), |head: Head| head.revenue as int);
    assert forall|query: int, revenue: bool| #[trigger] head_sum(heads, query, revenue) ==
        if query == supplier as int { head_sum(heads, supplier as int, revenue) } else { 0int } by {
        if query != supplier as int { lemma_sum_zero(heads, head_weight(query, revenue)); }
    }
}
pub proof fn lemma_heads_subset(before: Seq<Head>, after: Seq<Head>, removed: Head)
    requires after.to_multiset() == before.to_multiset().remove(removed),
    ensures forall|position: int| 0 <= position < after.len() ==> #[trigger] before.contains(after[position]),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    assert forall|position: int| 0 <= position < after.len() implies #[trigger] before.contains(after[position]) by {
        let value = after[position];
        assert(after.contains(value));
        assert(after.to_multiset().contains(value));
        assert(before.to_multiset().contains(value));
    }
}
pub proof fn lemma_heads_insert(before: Seq<Head>, after: Seq<Head>, inserted: Head)
    requires after.to_multiset() == before.to_multiset().insert(inserted),
    ensures forall|position: int| 0 <= position < after.len() ==> #[trigger] before.contains(after[position]) || after[position] == inserted,
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    assert forall|position: int| 0 <= position < after.len() implies #[trigger] before.contains(after[position]) || after[position] == inserted by {
        let value = after[position];
        assert(after.contains(value));
        assert(after.to_multiset().contains(value));
        if value != inserted { assert(before.to_multiset().contains(value)); }
    }
}
pub proof fn lemma_ready_subset(streams: HashMapWithView<usize, Stream>, before: Seq<Head>, after: Seq<Head>)
    requires heads_ready(streams, before),
        forall|position: int| 0 <= position < after.len() ==> #[trigger] before.contains(after[position]),
    ensures heads_ready(streams, after),
{
    assert forall|position: int| 0 <= position < after.len() implies {
        let head = #[trigger] after[position];
        streams@.contains_key(head.stream) &&
        (forall|offset: int| 0 <= offset < streams@[head.stream].cursor.remaining().len() ==>
            (#[trigger] streams@[head.stream].cursor.remaining()[offset]).0.0 > head.supplier as int)
    } by {
        assert(before.contains(after[position]));
        let index = choose|index: int| 0 <= index < before.len() && (#[trigger] before[index]) == after[position];
        assert(before[index] == after[position]);
    }
}
proof fn lemma_ready_insert(streams: HashMapWithView<usize, Stream>, before: Seq<Head>, after: Seq<Head>, head: Head)
    requires heads_ready(streams, before), after.to_multiset() == before.to_multiset().insert(head),
        streams@.contains_key(head.stream),
        forall|offset: int| 0 <= offset < streams@[head.stream].cursor.remaining().len() ==>
            (#[trigger] streams@[head.stream].cursor.remaining()[offset]).0.0 > head.supplier as int,
    ensures heads_ready(streams, after),
{
    lemma_heads_insert(before, after, head);
    assert forall|position: int| 0 <= position < after.len() implies {
        let value = #[trigger] after[position];
        streams@.contains_key(value.stream) &&
        (forall|offset: int| 0 <= offset < streams@[value.stream].cursor.remaining().len() ==>
            (#[trigger] streams@[value.stream].cursor.remaining()[offset]).0.0 > value.supplier as int)
    } by {
        assert(before.contains(after[position]) || after[position] == head);
        if after[position] != head {
            let index = choose|index: int| 0 <= index < before.len() && (#[trigger] before[index]) == after[position];
            assert(before[index] == after[position]);
        }
    }
}
pub proof fn lemma_filter_empty(totals: Seq<(i64, i128)>, value: i128)
    requires forall|position: int| 0 <= position < totals.len() ==> (#[trigger] totals[position]).1 != value,
    ensures winning_rows(totals, value).len() == 0,
    decreases totals.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if totals.len() > 0 { lemma_filter_empty(totals.drop_last(), value); }
}
pub proof fn lemma_maximum_step(totals: Seq<(i64, i128)>, maximum: Option<i128>, winners: Seq<i64>, supplier: i64, revenue: i128)
    requires maximum_matches(totals, maximum, winners),
    ensures maximum_matches(totals.push((supplier, revenue)),
        if maximum is None || revenue > maximum.unwrap() { Some(revenue) } else { maximum },
        if maximum is None || revenue > maximum.unwrap() { seq![supplier] }
        else if maximum == Some(revenue) { winners.push(supplier) } else { winners }),
{
    reveal_with_fuel(Seq::filter, 2);
    let after = totals.push((supplier, revenue));
    assert(after.drop_last() =~= totals);
    assert(after.last() == (supplier, revenue));
    let larger = maximum is None || revenue > maximum.unwrap();
    let value = if larger { revenue } else { maximum.unwrap() };
    assert forall|position: int| 0 <= position < after.len() implies (#[trigger] after[position]).1 <= value by {
        if position < totals.len() { assert(after[position] == totals[position]); }
    }
    if larger {
        assert forall|position: int| 0 <= position < totals.len() implies (#[trigger] totals[position]).1 != revenue by { }
        lemma_filter_empty(totals, revenue);
        assert(winning_rows(after, revenue) =~= seq![(supplier, revenue)]);
        assert(winning_keys(after, revenue) =~= seq![supplier]);
        assert(after[totals.len() as int].1 == value);
        assert(exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).1 == value);
    } else {
        let witness = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value;
        assert(after[witness].1 == value);
        assert(exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).1 == value);
        let selected = winning_rows(totals, value);
        if maximum == Some(revenue) {
            assert(winning_rows(after, value) =~= selected.push((supplier, revenue)));
            assert(selected.push((supplier, revenue)).map_values(|pair: (i64, i128)| pair.0) =~= winners.push(supplier));
            assert(winning_keys(after, value) == winners.push(supplier));
        } else {
            assert(winning_rows(after, value) =~= selected);
            assert(winning_keys(after, value) == winners);
        }
    }
}
#[verifier::spinoff_prover]
#[verifier::rlimit(30)]
fn consume_supplier(mut streams: HashMapWithView<usize, Stream>, mut heap: Vec<Head>, ids: &Vec<usize>, supplier: i64)
    -> (result: Option<(HashMapWithView<usize, Stream>, Vec<Head>, i128, Ghost<Seq<Head>>)>)
    requires heap_valid(heap@), heap.len() > 0, heap@[0].supplier == supplier,
        streams_valid(streams), heads_ready(streams, heap@), streams_scheduled(streams, heap@), positive_heads(heap@), heap.len() <= ids.len(),
        remaining(streams, ids@, heap@) >= 0,
        forall|id: usize| #[trigger] streams@.contains_key(id) <==> id < ids.len(),
        forall|position: int| 0 <= position < ids.len() ==> (#[trigger] ids@[position]) as int == position,
    ensures result matches Some(after) ==> heap_valid(after.1@) && streams_valid(after.0) && heads_ready(after.0, after.1@),
        result matches Some(after) ==> streams_scheduled(after.0, after.1@) && positive_heads(after.1@) && positive_heads(after.3@),
        result matches Some(after) ==> after.1.len() <= heap.len(),
        result matches Some(after) ==> remaining(after.0, ids@, after.1@) < remaining(streams, ids@, heap@) && remaining(after.0, ids@, after.1@) >= 0,
        result matches Some(after) ==> forall|id: usize| #[trigger] after.0@.contains_key(id) <==> id < ids.len(),
        result matches Some(after) ==> forall|position: int| 0 <= position < after.1.len() ==> (#[trigger] after.1@[position]).supplier > supplier,
        result matches Some(after) ==> after.2 == sum(after.3@, |head: Head| head.revenue as int) && after.3@.len() > 0,
        result matches Some(after) ==> forall|position: int| 0 <= position < after.3@.len() ==> (#[trigger] after.3@[position]).supplier == supplier,
        result matches Some(after) ==> forall|query: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, query, revenue) ==
            frontier_sum(after.0, ids@, after.1@, query, revenue) + head_sum(after.3@, query, revenue),
{
    let ghost original_heap = heap@;
    let ghost initial_streams = streams;
    let ghost work_before = remaining(streams, ids@, heap@);
    proof {
        assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).supplier >= supplier by {
            lemma_heap_min(heap@, position);
        }
    }
        let mut revenue = 0i128;
        let mut consumed = Ghost(Seq::<Head>::empty());
        while heap.len() > 0 && heap[0].supplier == supplier
            invariant heap_valid(heap@),
                remaining(streams, ids@, heap@) >= 0,
                heap.len() <= ids.len(), streams_valid(streams), heads_ready(streams, heap@),
                streams_scheduled(streams, heap@), positive_heads(heap@), positive_heads(consumed@),
                heap.len() <= original_heap.len(),
                forall|query: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, query, revenue) + head_sum(consumed@, query, revenue) ==
                    frontier_sum(initial_streams, ids@, original_heap, query, revenue),
                forall|id: usize| #[trigger] streams@.contains_key(id) <==> id < ids.len(),
                forall|position: int| 0 <= position < ids.len() ==> (#[trigger] ids@[position]) as int == position,
                remaining(streams, ids@, heap@) <= work_before,
                consumed@.len() > 0 ==> remaining(streams, ids@, heap@) < work_before,
                consumed@.len() == 0 ==> heap.len() > 0 && heap@[0].supplier == supplier,
                forall|position: int| 0 <= position < heap.len() ==> (#[trigger] heap@[position]).supplier >= supplier,
                revenue == sum(consumed@, |head: Head| head.revenue as int),
                forall|position: int| 0 <= position < consumed@.len() ==> (#[trigger] consumed@[position]).supplier == supplier,
            decreases remaining(streams, ids@, heap@),
        {
            let ghost before_heap = heap@;
            let ghost before_streams = streams;
            let ghost previously_consumed = consumed@;
            let head = match pop(&mut heap) { Some(value) => value, None => return None };
            let ghost popped_heap = heap@;
            proof {
                lemma_heads_subset(before_heap, heap@, head);
                lemma_ready_subset(streams, before_heap, heap@);
                assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).count@ > 0 by {
                    assert(before_heap.contains(heap@[position]));
                    let index = choose|index: int| 0 <= index < before_heap.len() && (#[trigger] before_heap[index]) == heap@[position];
                }
                lemma_sum_push(consumed@, head, |head: Head| head.revenue as int);
                assert(head.supplier == supplier);
                assert(before_heap.contains(head));
                assert forall|query: int, revenue: bool| #[trigger] head_sum(before_heap, query, revenue) ==
                    head_sum(popped_heap, query, revenue) + head_weight(query, revenue)(head) by {
                    lemma_head_remove_sum(before_heap, popped_heap, head, query, revenue);
                    lemma_sum_push(previously_consumed, head, head_weight(query, revenue));
                }
                assert(head.stream < ids.len());
                assert(ids@[head.stream as int] == head.stream);
                assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).supplier >= supplier by {
                    assert(before_heap.contains(heap@[position]));
                    let index = choose|index: int| 0 <= index < before_heap.len() && (#[trigger] before_heap[index]) == heap@[position];
                }
            }
            revenue = match add(revenue, head.revenue) { Some(value) => value, None => return None };
            consumed = Ghost(consumed@.push(head));
            let mut stream = match streams.remove(&head.stream) { Some(value) => value, None => return None };
            let ghost stream_before = stream;
            let next_head = match next(&mut stream, head.stream) { Some(value) => value, None => return None };
            streams.insert(head.stream, stream);
            proof {
                assert(streams_valid(streams));
                assert forall|position: int| 0 <= position < heap.len() implies {
                    let other = #[trigger] heap@[position];
                    streams@.contains_key(other.stream) &&
                    (forall|offset: int| 0 <= offset < streams@[other.stream].cursor.remaining().len() ==>
                        (#[trigger] streams@[other.stream].cursor.remaining()[offset]).0.0 > other.supplier as int)
                } by {
                    if heap@[position].stream == head.stream {
                        assert forall|offset: int| 0 <= offset < streams@[head.stream].cursor.remaining().len() implies
                            (#[trigger] streams@[head.stream].cursor.remaining()[offset]).0.0 > heap@[position].supplier as int by {
                            let value = streams@[head.stream].cursor.remaining()[offset];
                            let index = choose|index: int| 0 <= index < stream_before.cursor.remaining().len() && (#[trigger] stream_before.cursor.remaining()[index]) == value;
                        }
                    }
                }
            }
            if let Some(value) = next_head {
                let ghost short_heap = heap@;
                proof {
                    let index = choose|index: int| 0 <= index < stream_before.cursor.remaining().len() &&
                        (#[trigger] stream_before.cursor.remaining()[index]).0.0 == value.supplier as int;
                    assert(value.supplier > supplier);
                }
                push(&mut heap, value);
                proof {
                    assert forall|query: int, revenue: bool| #[trigger] head_sum(heap@, query, revenue) ==
                        head_sum(popped_heap, query, revenue) + head_weight(query, revenue)(value) by {
                        lemma_head_insert_sum(popped_heap, heap@, value, query, revenue);
                    }
                    lemma_ready_insert(streams, short_heap, heap@, value);
                    lemma_heads_insert(short_heap, heap@, value);
                    assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).count@ > 0 by {
                        if heap@[position] != value {
                            assert(short_heap.contains(heap@[position]));
                            let index = choose|index: int| 0 <= index < short_heap.len() && (#[trigger] short_heap[index]) == heap@[position];
                        }
                    }
                    assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).supplier >= supplier by {
                        if heap@[position] != value {
                            assert(short_heap.contains(heap@[position]));
                            let index = choose|index: int| 0 <= index < short_heap.len() && (#[trigger] short_heap[index]) == heap@[position];
                        }
                    }
                }
            }
            proof {
                lemma_remaining_step(before_streams, streams, ids@, head.stream, before_heap, heap@);
                lemma_scheduled_step(before_streams, streams, before_heap, heap@, head, next_head);
                assert forall|query: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, query, revenue) + head_sum(consumed@, query, revenue) ==
                    frontier_sum(initial_streams, ids@, original_heap, query, revenue) by {
                    lemma_stream_sums_change(before_streams, streams, ids@, head.stream, query, revenue);
                    assert(stream_sum(before_streams@[head.stream], query, revenue) == stream_sum(stream_before, query, revenue));
                    if next_head is Some {
                        assert(stream_sum(stream_before, query, revenue) == stream_sum(streams@[head.stream], query, revenue) + head_weight(query, revenue)(next_head.unwrap()));
                    } else {
                        assert(stream_sum(stream_before, query, revenue) == 0);
                        assert(streams@[head.stream].cursor.remaining().len() == 0);
                    }
                    assert(frontier_sum(before_streams, ids@, before_heap, query, revenue) + head_sum(previously_consumed, query, revenue) == frontier_sum(initial_streams, ids@, original_heap, query, revenue));
                    lemma_sum_push(previously_consumed, head, head_weight(query, revenue));
                }
            }
        }
        proof {
            assert(consumed@.len() > 0);
            assert(remaining(streams, ids@, heap@) < work_before);
            assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).supplier > supplier by {
                lemma_heap_min(heap@, position);
            }
        }
    Some((streams, heap, revenue, consumed))
}
#[verifier::spinoff_prover]
#[verifier::rlimit(30)]
pub fn execute(facts: &Facts, dimensions: &Dimensions, lower: i32, upper: i32, Ghost(db): Ghost<Db>)
    -> (result: Option<Vec<OutputRow>>)
    requires facts_match(*facts, db.lineitem), dimensions_match(*dimensions, db.supplier),
    ensures result matches Some(rows) ==> query_result(db, params(lower as int, upper as int), out_view(rows@)),
{
    let mut output = Vec::new();
    if lower >= upper {
        proof {
            crate::database_proofs::lemma_empty_range(db, params(lower as int, upper as int));
            assert(out_view(output@) =~= Seq::<crate::gen_spec::OutRow>::empty());
        }
        return Some(output);
    }
    let first_bin = bin(lower);
    let last_bin = bin(upper - 1);
    let mut buckets = facts.cursor_from(&Key::number(first_bin));
    let mut streams: HashMapWithView<usize, Stream> = HashMapWithView::new();
    let mut ids = Vec::new();
    let mut heap = Vec::new();
    let shared_facts = facts.share();
    let ghost entries = facts.entries();
    let mut bin_position = Ghost(verus_prooflib::ordered::lower_position(entries, number_key(first_bin as int)));
    let mut finished = false;
    proof {
        facts.model();
        verus_prooflib::ordered::lemma_lower_suffix(facts.entries(), number_key(first_bin as int));
        assert(buckets.remaining() == verus_prooflib::ordered::lower_rows(facts.entries(), number_key(first_bin as int)));
        assert forall|position: int| 0 <= position < buckets.remaining().len() implies
            facts.at((#[trigger] buckets.remaining()[position]).0) == Some(buckets.remaining()[position].1) by {
            let start = verus_prooflib::ordered::lower_position(facts.entries(), number_key(first_bin as int));
            assert(buckets.remaining()[position] == facts.entries()[start + position]);
        }
        assert forall|supplier: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, supplier, revenue) ==
            sum(entries.take(bin_position@), facts_window_weight(supplier, lower as int, upper as int, revenue)) by {
            let weight = facts_window_weight(supplier, lower as int, upper as int, revenue);
            assert forall|position: int| 0 <= position < entries.take(bin_position@).len() implies weight(#[trigger] entries.take(bin_position@)[position]) == 0 by {
                assert(entries.take(bin_position@)[position] == entries[position]);
                assert(verus_prooflib::ordered::key_lt(entries[position].0, number_key(first_bin as int)));
                assert(entries[position].0.1 == Seq::<u8>::empty());
                assert(entries[position].0.0 < first_bin as int);
                lemma_bucket_range_zero(facts, entries[position], supplier, lower as int, upper as int, revenue);
            }
            lemma_sum_zero(entries.take(bin_position@), weight);
        }
    }
    while !finished
        invariant lower < upper, facts.wf(), facts_shape(*facts), heap_valid(heap@), heap.len() <= ids.len(),
            first_bin as int == bucket(lower as int), last_bin as int == bucket(upper as int - 1), buckets.work() >= 0,
            facts_match(*facts, db.lineitem), entries == facts.entries(), 0 <= bin_position@ <= entries.len(),
            buckets.remaining() == entries.skip(bin_position@),
            !finished ==> forall|supplier: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, supplier, revenue) ==
                sum(entries.take(bin_position@), facts_window_weight(supplier, lower as int, upper as int, revenue)),
            finished ==> forall|supplier: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, supplier, revenue) ==
                facts_window(*facts, supplier, lower as int, upper as int, revenue),
            streams_valid(streams), heads_ready(streams, heap@),
            positive_heads(heap@), streams_scheduled(streams, heap@),
            forall|id: usize| #[trigger] streams@.contains_key(id) <==> id < ids.len(),
            forall|position: int| 0 <= position < ids.len() ==> (#[trigger] ids@[position]) as int == position,
            forall|position: int| 0 <= position < buckets.remaining().len() ==>
                facts.at((#[trigger] buckets.remaining()[position]).0) == Some(buckets.remaining()[position].1),
        decreases if finished { 0int } else { buckets.work() + 1 },
    {
        let ghost pending = buckets.remaining();
        let (key, suppliers) = match buckets.next() { Some(value) => value, None => {
            proof {
                assert(pending.len() == 0);
                assert(bin_position@ == entries.len());
                assert(entries.take(bin_position@) =~= entries);
            }
            finished = true;
            continue;
        } };
        proof {
            assert(facts.at(key.view()) == Some(*suppliers));
            assert(key.view() == number_key(key.number as int));
            assert forall|position: int| 0 <= position < buckets.remaining().len() implies
                facts.at((#[trigger] buckets.remaining()[position]).0) == Some(buckets.remaining()[position].1) by {
                assert(buckets.remaining()[position] == pending[position + 1]);
            }
        }
        if key.number > last_bin {
            proof {
                facts.model();
                assert forall|supplier: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, supplier, revenue) ==
                    facts_window(*facts, supplier, lower as int, upper as int, revenue) by {
                    let weight = facts_window_weight(supplier, lower as int, upper as int, revenue);
                    assert forall|position: int| 0 <= position < pending.len() implies weight(#[trigger] pending[position]) == 0 by {
                        assert(pending[position] == entries[bin_position@ + position]);
                        assert(pending[0] == entries[bin_position@]);
                        if position > 0 {
                            assert(verus_prooflib::ordered::key_lt(pending[0].0, pending[position].0));
                            assert(pending[position].0.1 == Seq::<u8>::empty());
                            verus_prooflib::ordered::lemma_key_order(pending[0].0, pending[position].0);
                        }
                        assert(pending[position].0.0 >= key.number as int);
                        lemma_bucket_range_zero(facts, pending[position], supplier, lower as int, upper as int, revenue);
                    }
                    lemma_sum_zero(pending, weight);
                    assert(entries =~= entries.take(bin_position@) + pending);
                    lemma_sum_concat(entries.take(bin_position@), pending, weight);
                    lemma_sum_take(entries, bin_position@, weight);
                }
            }
            bin_position = Ghost(bin_position@ + 1);
            finished = true;
            continue;
        }
        if ids.len() == usize::MAX { return None; }
        let full = lower as i128 <= (key.number as i128) * 256 && upper as i128 >= (key.number as i128 + 1) * 256;
        let mut stream = Stream { cursor: suppliers.cursor(), full, lower, upper };
        let shared_suppliers = suppliers.share();
        proof {
            suppliers.model();
            assert forall|position: int| 0 <= position < stream.cursor.remaining().len() implies {
                let profile = (#[trigger] stream.cursor.remaining()[position]).1;
                stream.cursor.remaining()[position].0.1 == Seq::<u8>::empty() &&
                (forall|offset: int| 0 <= offset < profile.days.len() ==> (#[trigger] profile.days@[offset]).count > 0) &&
                (stream.full ==> profile.count as int == selected_count(profile, stream.lower as int, stream.upper as int) &&
                    profile.revenue as int == selected_revenue(profile, stream.lower as int, stream.upper as int)) &&
                (forall|left: int, right: int| #![trigger profile.days@[left], profile.days@[right]]
                    0 <= left < right < profile.days.len() ==> profile.days@[left].shipdate < profile.days@[right].shipdate)
            } by {
                let supplier_key = suppliers.entries()[position].0;
                assert(suppliers.at(supplier_key) == Some(suppliers.entries()[position].1));
                assert(supplier_key.1 == Seq::<u8>::empty());
                assert(profile_at(*facts, key.number as int, supplier_key.0) == Some(suppliers.entries()[position].1));
                assert(profile_shape(suppliers.entries()[position].1, key.number as int));
                if full { lemma_profile_full(suppliers.entries()[position].1, key.number as int, lower as int, upper as int); }
            }
            assert forall|left: int, right: int| #![trigger stream.cursor.remaining()[left], stream.cursor.remaining()[right]]
                0 <= left < right < stream.cursor.remaining().len() implies
                    stream.cursor.remaining()[left].0.0 < stream.cursor.remaining()[right].0.0 by {
                assert(suppliers.entries()[left].0.1 == Seq::<u8>::empty());
                assert(suppliers.entries()[right].0.1 == Seq::<u8>::empty());
                verus_prooflib::ordered::lemma_key_order(suppliers.entries()[left].0, suppliers.entries()[right].0);
            }
        }
        let id = ids.len();
        let ghost initial_bucket = stream;
        let head = match next(&mut stream, id) { Some(value) => value, None => return None };
        let ghost before_heap = heap@;
        let ghost before_streams = streams;
        let ghost new_stream = stream;
        let ghost before_ids = ids@;
        streams.insert(id, stream);
        proof {
            assert(streams_valid(streams));
            assert(heads_ready(streams, before_heap));
        }
        if let Some(head) = head {
            push(&mut heap, head);
            proof {
                lemma_ready_insert(streams, before_heap, heap@, head);
                lemma_heads_insert(before_heap, heap@, head);
                assert forall|position: int| 0 <= position < heap.len() implies (#[trigger] heap@[position]).count@ > 0 by {
                    if heap@[position] != head {
                        assert(before_heap.contains(heap@[position]));
                        let index = choose|index: int| 0 <= index < before_heap.len() && (#[trigger] before_heap[index]) == heap@[position];
                    }
                }
            }
        }
        proof {
            broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
            assert(heap@.to_multiset() == match head { Some(value) => before_heap.to_multiset().insert(value), None => before_heap.to_multiset() });
            assert forall|query_id: usize| #[trigger] streams@.contains_key(query_id) && streams@[query_id].cursor.remaining().len() > 0 implies
                exists|position: int| 0 <= position < heap.len() && (#[trigger] heap@[position]).stream == query_id by {
                if query_id == id {
                    assert(head is Some);
                    assert(heap@.to_multiset().contains(head.unwrap()));
                    assert(heap@.contains(head.unwrap()));
                    let position = choose|position: int| 0 <= position < heap.len() && (#[trigger] heap@[position]) == head.unwrap();
                } else {
                    let index = choose|index: int| 0 <= index < before_heap.len() && (#[trigger] before_heap[index]).stream == query_id;
                    let value = before_heap[index];
                    assert(before_heap.contains(value));
                    assert(heap@.to_multiset().count(value) > 0);
                    assert(heap@.contains(value));
                    let position = choose|position: int| 0 <= position < heap.len() && (#[trigger] heap@[position]) == value;
                }
            }
        }
        ids.push(id);
        proof {
            assert(entries[bin_position@] == (key.view(), *suppliers));
            assert forall|supplier: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, supplier, revenue) ==
                sum(entries.take(bin_position@ + 1), facts_window_weight(supplier, lower as int, upper as int, revenue)) by {
                let old_values = frontier_weight(before_streams, supplier, revenue);
                let new_values = frontier_weight(streams, supplier, revenue);
                assert forall|position: int| 0 <= position < before_ids.len() implies old_values(#[trigger] before_ids[position]) == new_values(before_ids[position]) by {
                    assert(before_ids[position] < id);
                }
                lemma_sum_ext(before_ids, old_values, new_values);
                lemma_sum_push(before_ids, id, new_values);
                if head is Some {
                    lemma_head_insert_sum(before_heap, heap@, head.unwrap(), supplier, revenue);
                    assert(stream_sum(initial_bucket, supplier, revenue) == stream_sum(new_stream, supplier, revenue) + head_weight(supplier, revenue)(head.unwrap()));
                } else { assert(stream_sum(initial_bucket, supplier, revenue) == 0); assert(new_stream.cursor.remaining().len() == 0); }
                assert(stream_sum(initial_bucket, supplier, revenue) == facts_window_weight(supplier, lower as int, upper as int, revenue)(entries[bin_position@]));
                assert(frontier_sum(before_streams, before_ids, before_heap, supplier, revenue) == sum(entries.take(bin_position@), facts_window_weight(supplier, lower as int, upper as int, revenue)));
                lemma_sum_take(entries, bin_position@, facts_window_weight(supplier, lower as int, upper as int, revenue));
            }
        }
        bin_position = Ghost(bin_position@ + 1);
    }
    let mut maximum: Option<i128> = None;
    let mut winners = Vec::new();
    let mut finalized = Ghost(Seq::<(i64, i128)>::empty());
    let mut processed_heads = Ghost(Seq::<Head>::empty());
    proof { lemma_sum_nonneg(ids@, stream_sizes(streams)); }
    while heap.len() > 0
        invariant heap_valid(heap@), maximum_matches(finalized@, maximum, winners@),
            facts_match(*facts, db.lineitem), dimensions_match(*dimensions, db.supplier), facts.wf(), lower < upper,
            positive_heads(processed_heads@),
            forall|supplier: int, revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, supplier, revenue) + head_sum(processed_heads@, supplier, revenue) == facts_window(*facts, supplier, lower as int, upper as int, revenue),
            forall|supplier: int| #[trigger] head_sum(processed_heads@, supplier, false) > 0 <==> has_group(finalized@, supplier),
            forall|position: int| 0 <= position < finalized@.len() ==> {
                let pair = #[trigger] finalized@[position];
                window_sum(db.lineitem, pair.0 as int, lower as int, upper as int, false) > 0 &&
                pair.1 as int == window_sum(db.lineitem, pair.0 as int, lower as int, upper as int, true)
            },
            remaining(streams, ids@, heap@) >= 0,
            heap.len() <= ids.len(), streams_valid(streams), heads_ready(streams, heap@),
            positive_heads(heap@), streams_scheduled(streams, heap@),
            forall|id: usize| #[trigger] streams@.contains_key(id) <==> id < ids.len(),
            forall|position: int| 0 <= position < ids.len() ==> (#[trigger] ids@[position]) as int == position,
            forall|position: int, done: int| #![trigger heap@[position], finalized@[done]]
                0 <= position < heap.len() && 0 <= done < finalized@.len() ==> heap@[position].supplier > finalized@[done].0,
            forall|left: int, right: int| #![trigger finalized@[left], finalized@[right]]
                0 <= left < right < finalized@.len() ==> finalized@[left].0 < finalized@[right].0,
        decreases remaining(streams, ids@, heap@),
    {
        let supplier = heap[0].supplier;
        let ghost old_streams = streams;
        let ghost old_heap = heap@;
        let ghost previous_heads = processed_heads@;
        let ghost previous_totals = finalized@;
        proof {
            assert forall|query: int, selected_revenue: bool| #[trigger] frontier_sum(old_streams, ids@, old_heap, query, selected_revenue) + head_sum(previous_heads, query, selected_revenue) == facts_window(*facts, query, lower as int, upper as int, selected_revenue) by { }
            assert forall|query: int| #[trigger] head_sum(previous_heads, query, false) > 0 <==> has_group(previous_totals, query) by { }
        }
        let (updated_streams, updated_heap, revenue, consumed) = match consume_supplier(streams, heap, &ids, supplier) {
            Some(value) => value, None => return None,
        };
        streams = updated_streams;
        heap = updated_heap;
        proof {
            lemma_maximum_step(finalized@, maximum, winners@, supplier, revenue);
            lemma_head_group(consumed@, supplier);
            assert(!has_group(finalized@, supplier as int));
            lemma_sum_nonneg(previous_heads, head_weight(supplier as int, false));
            assert(head_sum(previous_heads, supplier as int, false) == 0);
            lemma_head_empty_key(previous_heads, supplier as int);
            lemma_frontier_zero(streams, ids@, heap@, supplier as int, false);
            lemma_frontier_zero(streams, ids@, heap@, supplier as int, true);
            lemma_facts_window(facts, db.lineitem, supplier as int, lower as int, upper as int, false);
            lemma_facts_window(facts, db.lineitem, supplier as int, lower as int, upper as int, true);
            assert(frontier_sum(old_streams, ids@, old_heap, supplier as int, false) == frontier_sum(streams, ids@, heap@, supplier as int, false) + head_sum(consumed@, supplier as int, false));
            assert(frontier_sum(old_streams, ids@, old_heap, supplier as int, true) == frontier_sum(streams, ids@, heap@, supplier as int, true) + head_sum(consumed@, supplier as int, true));
            assert(frontier_sum(old_streams, ids@, old_heap, supplier as int, false) + head_sum(previous_heads, supplier as int, false) == facts_window(*facts, supplier as int, lower as int, upper as int, false));
            assert(frontier_sum(old_streams, ids@, old_heap, supplier as int, true) + head_sum(previous_heads, supplier as int, true) == facts_window(*facts, supplier as int, lower as int, upper as int, true));
            assert(window_sum(db.lineitem, supplier as int, lower as int, upper as int, false) == head_sum(consumed@, supplier as int, false));
            assert(window_sum(db.lineitem, supplier as int, lower as int, upper as int, true) == revenue as int);
        }
        let larger = match maximum { Some(value) => revenue > value, None => true };
        if larger { maximum = Some(revenue); winners.clear(); }
        if maximum == Some(revenue) { winners.push(supplier); }
        finalized = Ghost(finalized@.push((supplier, revenue)));
        processed_heads = Ghost(processed_heads@ + consumed@);
        proof {
            assert forall|position: int| 0 <= position < processed_heads@.len() implies (#[trigger] processed_heads@[position]).count@ > 0 by {
                if position < previous_heads.len() { assert(processed_heads@[position] == previous_heads[position]); }
                else { assert(processed_heads@[position] == consumed@[position - previous_heads.len()]); }
            }
            assert forall|query: int, selected_revenue: bool| #[trigger] frontier_sum(streams, ids@, heap@, query, selected_revenue) + head_sum(processed_heads@, query, selected_revenue) == facts_window(*facts, query, lower as int, upper as int, selected_revenue) by {
                lemma_sum_concat(previous_heads, consumed@, head_weight(query, selected_revenue));
                assert(frontier_sum(old_streams, ids@, old_heap, query, selected_revenue) == frontier_sum(streams, ids@, heap@, query, selected_revenue) + head_sum(consumed@, query, selected_revenue));
            }
            assert forall|query: int| #[trigger] head_sum(processed_heads@, query, false) > 0 <==> has_group(finalized@, query) by {
                lemma_head_group(consumed@, supplier);
                lemma_sum_concat(previous_heads, consumed@, head_weight(query, false));
                lemma_sum_nonneg(previous_heads, head_weight(query, false));
                lemma_group_push(previous_totals, (supplier, revenue), query);
                assert(head_sum(previous_heads, query, false) > 0 <==> has_group(previous_totals, query));
                assert(head_sum(consumed@, query, false) == if query == supplier as int { head_sum(consumed@, supplier as int, false) } else { 0int });
            }
        }
    }
    proof {
        assert forall|query: int| #[trigger] has_group(finalized@, query) <==> window_sum(db.lineitem, query, lower as int, upper as int, false) > 0 by {
            lemma_frontier_zero(streams, ids@, heap@, query, false);
            lemma_facts_window(facts, db.lineitem, query, lower as int, upper as int, false);
        }
        assert(groups_match(finalized@, db, lower as int, upper as int));
    }
    let revenue = match maximum { Some(value) => value, None => {
        proof {
            assert forall|query: int| #[trigger] window_sum(db.lineitem, query, lower as int, upper as int, false) == 0 by {
                assert(!has_group(finalized@, query));
                lemma_sum_nonneg(db.lineitem, window_weight(query, lower as int, upper as int, false));
            }
            lemma_no_groups(db, params(lower as int, upper as int));
            assert(out_view(output@) =~= Seq::<crate::gen_spec::OutRow>::empty());
        }
        return Some(output);
    } };
    proof { crate::result_proofs::lemma_winners_sorted(finalized@, revenue); }
    let output = match crate::emission::emit(dimensions, &winners, revenue) { Some(value) => value, None => return None };
    proof {
        broadcast use vstd::multiset::group_multiset_axioms;
        assert forall|row: crate::gen_spec::OutRow| #[trigger] out_view(output@).to_multiset().count(row) == crate::gen_spec::rows(db, params(lower as int, upper as int)).to_multiset().count(row) by {
            crate::result_proofs::lemma_emitted_count(dimensions, db.supplier, winners@, revenue as int, row);
            crate::result_proofs::lemma_sql_count(db, params(lower as int, upper as int), finalized@, winners@, revenue, row);
        }
        assert(out_view(output@).to_multiset() =~= crate::gen_spec::rows(db, params(lower as int, upper as int)).to_multiset());
        crate::database_proofs::lemma_execute_ok(db, params(lower as int, upper as int), out_view(output@));
    }
    Some(output)
}
}
