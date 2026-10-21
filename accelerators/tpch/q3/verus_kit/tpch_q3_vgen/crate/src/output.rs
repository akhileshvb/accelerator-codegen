use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Cursor, Key, KeyView, Index, ordered_rows, key_lt};
use crate::paged::{Paged, PageCursor, entry_sorted, Ent};
use crate::rank_ent::*;
use verus_prooflib::IntegerTripleMap;
use crate::kernel::State;
use crate::columns::*;
use crate::model::*;
use crate::gen_spec::*;
use crate::gen_obligations::{params, out_view};
use crate::values::*;
use verus_prooflib::wide::{self, Wide};
use crate::semantics::*;
use crate::read_proofs::*;

verus! {

pub type WRow = (i64, Wide, i32, i64);

pub struct Stream {
    cursor: PageCursor<RankEnt>,
    head: RankEnt,
}

spec fn stream_has(stream: Stream, rank: KeyView) -> bool {
    stream.head.kv() == rank || (exists|position: int| #![trigger stream.cursor.remaining()[position]]
        0 <= position < stream.cursor.remaining().len() && stream.cursor.remaining()[position].kv() == rank)
}

spec fn pending(streams: Seq<Stream>, rank: KeyView) -> bool {
    exists|position: int| #![trigger streams[position]] 0 <= position < streams.len() && stream_has(streams[position], rank)
}

spec fn stream_ordered(stream: Stream) -> bool {
    &&& stream.cursor.well_formed()
    &&& entry_sorted(stream.cursor.remaining())
    &&& forall|position: int| #![trigger stream.cursor.remaining()[position]]
        0 <= position < stream.cursor.remaining().len() ==> key_lt(stream.head.kv(), stream.cursor.remaining()[position].kv())
}

/// A stream's head holds the largest bound of what is still to come.
proof fn lemma_upper_of(entry: RankEnt, rank: KeyView)
    requires entry.kv() == rank || key_lt(entry.kv(), rank),
    ensures -1 - entry.upper as int <= rank.0,
{}

fn open_stream(index: &Paged<RankEnt>) -> (result: Option<Stream>)
    requires index.wf(),
    ensures result matches Some(stream) ==> stream_ordered(stream),
        forall|rank: KeyView| #![trigger index.at(rank)] index.at(rank).is_some() ==> result.is_some() && stream_has(result.unwrap(), rank),
{
    proof { index.model(); }
    let mut cursor = index.cursor();
    if let Some(head) = cursor.next() {
        let stream = Stream { cursor, head };
        proof {
            let rows = index.entries();
            assert(stream.cursor.remaining() == rows.skip(1));
            assert(stream.head == rows[0]);
            assert forall|first: int, last: int| #![trigger stream.cursor.remaining()[first], stream.cursor.remaining()[last]]
                0 <= first < last < stream.cursor.remaining().len() implies key_lt(stream.cursor.remaining()[first].kv(), stream.cursor.remaining()[last].kv()) by {
                assert(stream.cursor.remaining()[first] == rows[first + 1]);
                assert(stream.cursor.remaining()[last] == rows[last + 1]);
            }
            assert forall|position: int| #![trigger stream.cursor.remaining()[position]]
                0 <= position < stream.cursor.remaining().len() implies key_lt(stream.head.kv(), stream.cursor.remaining()[position].kv()) by {
                assert(stream.cursor.remaining()[position] == rows[position + 1]);
            }
            assert forall|rank: KeyView| #![trigger index.at(rank)] index.at(rank).is_some() implies stream_has(stream, rank) by {
                assert(index.has_key(rank));
                let entry = index.pick(rank);
                assert(index.holds(entry) && entry.kv() == rank);
                assert(rows.contains(entry));
                let at = choose|at: int| 0 <= at < rows.len() && rows[at] == entry;
                if at > 0 { assert(stream.cursor.remaining()[at - 1] == rows[at]); }
            }
        }
        Some(stream)
    } else {
        proof {
            let rows = index.entries();
            assert forall|rank: KeyView| #![trigger index.at(rank)] index.at(rank).is_some() implies false by {
                assert(index.has_key(rank));
                let entry = index.pick(rank);
                assert(rows.contains(entry));
            }
        }
        None
    }
}

fn advance(mut stream: Stream) -> (result: Option<Stream>)
    requires stream_ordered(stream),
    ensures result matches Some(next) ==> stream_ordered(next)
        && next.cursor.remaining().len() + 1 == stream.cursor.remaining().len(),
        forall|rank: KeyView| #![trigger stream_has(stream, rank)]
            stream_has(stream, rank) && rank != stream.head.kv() ==> result.is_some() && stream_has(result.unwrap(), rank),
{
    let ghost original = stream;
    if let Some(head) = stream.cursor.next() {
        stream.head = head;
        proof {
            let old_rows = original.cursor.remaining();
            assert(stream.cursor.remaining() == old_rows.skip(1));
            assert(head == old_rows[0]);
            assert forall|first: int, last: int| #![trigger stream.cursor.remaining()[first], stream.cursor.remaining()[last]]
                0 <= first < last < stream.cursor.remaining().len() implies key_lt(stream.cursor.remaining()[first].kv(), stream.cursor.remaining()[last].kv()) by {
                assert(stream.cursor.remaining()[first] == old_rows[first + 1]);
                assert(stream.cursor.remaining()[last] == old_rows[last + 1]);
            }
            assert forall|position: int| #![trigger stream.cursor.remaining()[position]]
                0 <= position < stream.cursor.remaining().len() implies key_lt(stream.head.kv(), stream.cursor.remaining()[position].kv()) by {
                assert(stream.cursor.remaining()[position] == old_rows[position + 1]);
                assert(key_lt(old_rows[0].kv(), old_rows[position + 1].kv()));
            }
            assert forall|rank: KeyView| #![trigger stream_has(original, rank)]
                stream_has(original, rank) && rank != original.head.kv() implies stream_has(stream, rank) by {
                let source = choose|source: int| #![trigger old_rows[source]]
                    0 <= source < old_rows.len() && old_rows[source].kv() == rank;
                if source > 0 { assert(stream.cursor.remaining()[source - 1].kv() == rank); }
            }
        }
        Some(stream)
    } else {
        proof {
            assert forall|rank: KeyView| #![trigger stream_has(original, rank)]
                stream_has(original, rank) && rank != original.head.kv() implies false by {
                let source = choose|source: int| #![trigger original.cursor.remaining()[source]]
                    0 <= source < original.cursor.remaining().len() && original.cursor.remaining()[source].kv() == rank;
            }
        }
        None
    }
}

proof fn lemma_pending_remove(streams: Seq<Stream>, position: int, rank: KeyView)
    requires 0 <= position < streams.len(), pending(streams, rank),
    ensures pending(streams.remove(position), rank) || stream_has(streams[position], rank),
{
    let source = choose|source: int| #![trigger streams[source]] 0 <= source < streams.len() && stream_has(streams[source], rank);
    if source < position { assert(streams.remove(position)[source] == streams[source]); }
    else if source > position { assert(streams.remove(position)[source - 1] == streams[source]); }
}

proof fn lemma_pending_push(streams: Seq<Stream>, stream: Stream, rank: KeyView)
    ensures pending(streams.push(stream), rank) == (pending(streams, rank) || stream_has(stream, rank)),
{
    let after = streams.push(stream);
    if pending(after, rank) {
        let source = choose|source: int| #![trigger after[source]] 0 <= source < after.len() && stream_has(after[source], rank);
        if source < streams.len() { assert(streams[source] == after[source]); }
    }
    if pending(streams, rank) {
        let source = choose|source: int| #![trigger streams[source]] 0 <= source < streams.len() && stream_has(streams[source], rank);
        assert(after[source] == streams[source]);
    }
    assert(after[streams.len() as int] == stream);
}

closed spec fn remaining(streams: Seq<Stream>) -> int {
    sum(streams, |stream: Stream| stream.cursor.remaining().len() as int + 1)
}

pub fn before(left: WRow, right: WRow) -> (result: bool)
    ensures result == ext_lt(output_row(left), output_row(right)),
{
    wide::lt(right.1, left.1) || (wide::eq(left.1, right.1) && (left.2 < right.2 || (left.2 == right.2
        && (left.0 < right.0 || (left.0 == right.0 && left.3 < right.3)))))
}

pub open spec fn output_row(row: WRow) -> OutRow {
    OutRow { l_orderkey: row.0 as int, revenue: row.1.int(), o_orderdate: row.2 as int, o_shippriority: row.3 as int }
}

pub open spec fn wide_out(rows: Seq<WRow>) -> Seq<OutRow> {
    rows.map_values(|row: WRow| output_row(row))
}

pub open spec fn output_sorted(rows: Seq<WRow>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]] 0 <= left < right < rows.len() ==>
        !ext_lt(output_row(rows[right]), output_row(rows[left]))
}

pub open spec fn output_distinct(rows: Seq<WRow>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]] 0 <= left < right < rows.len() ==>
        !same_group(output_row(rows[left]), output_row(rows[right]))
}

pub open spec fn top_dominates(rows: Seq<WRow>, row: WRow) -> bool {
    rows.contains(row) || (rows.len() == 10 && !ext_lt(output_row(row), output_row(rows[9])))
}

pub open spec fn top_covers(rows: Seq<WRow>, row: OutRow) -> bool {
    (exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && output_row(rows[position]) == row)
        || (rows.len() == 10 && !ext_lt(row, output_row(rows[9])))
}

pub proof fn lemma_retention_cover(original: Seq<WRow>, result: Seq<WRow>)
    requires original.len() <= 10, result.len() <= 10,
        original.len() == 10 ==> result.len() == 10 && !ext_lt(output_row(original[9]), output_row(result[9])),
        forall|candidate: WRow| #![trigger top_dominates(result, candidate)]
            top_dominates(original, candidate) ==> top_dominates(result, candidate),
    ensures forall|candidate: OutRow| #![trigger top_covers(result, candidate)] top_covers(original, candidate) ==> top_covers(result, candidate),
{
    assert forall|candidate: OutRow| #![trigger top_covers(result, candidate)] top_covers(original, candidate) implies top_covers(result, candidate) by {
        if exists|position: int| #![trigger original[position]] 0 <= position < original.len() && output_row(original[position]) == candidate {
            let position = choose|position: int| #![trigger original[position]] 0 <= position < original.len() && output_row(original[position]) == candidate;
            let row = original[position];
            assert(top_dominates(original, row));
            assert(top_dominates(result, row));
            if result.contains(row) {
                let source = choose|source: int| #![trigger result[source]] 0 <= source < result.len() && result[source] == row;
            }
        } else {
            assert(original.len() == 10 && !ext_lt(candidate, output_row(original[9])));
        }
    }
}

pub proof fn lemma_top_insert(original: Seq<WRow>, result: Seq<WRow>,
    row: WRow, position: int)
    requires original.len() <= 10, 0 <= position < 10, position <= original.len(),
        output_sorted(original), output_sorted(result),
        forall|source: int| #![trigger original[source]] position <= source < original.len() ==>
            !ext_lt(output_row(original[source]), output_row(row)),
        result == if original.len() < 10 { original.insert(position, row) } else { original.insert(position, row).take(10) },
    ensures top_dominates(result, row),
        original.len() == 10 ==> result.len() == 10 && !ext_lt(output_row(original[9]), output_row(result[9])),
        forall|candidate: WRow| #![trigger top_dominates(result, candidate)]
            top_dominates(original, candidate) ==> top_dominates(result, candidate),
{
    let precedes = |left: WRow, right: WRow| !ext_lt(output_row(right), output_row(left));
    assert forall|left: WRow, middle: WRow, right: WRow|
        #![trigger precedes(left, middle), precedes(middle, right)]
        precedes(left, middle) && precedes(middle, right) implies precedes(left, right) by {}
    verus_prooflib::lemma_top_prefix_insert(original, result, row, position, 10, precedes);
    if original.len() == 10 {
        if position == 9 { assert(result[9] == row); }
        else { assert(result[9] == original[8]); }
    }
    assert forall|candidate: WRow| #![trigger top_dominates(result, candidate)]
        top_dominates(original, candidate) implies top_dominates(result, candidate) by {
        assert(verus_prooflib::prefix_dominates(original, candidate, 10, precedes));
        assert(verus_prooflib::prefix_dominates(result, candidate, 10, precedes));
    }
}

pub fn retain(rows: &mut Vec<WRow>, row: WRow)
    requires old(rows).len() <= 10,
    ensures final(rows).len() <= 10,
        output_sorted(old(rows)@) ==> top_covers(final(rows)@, output_row(row)),
        output_sorted(old(rows)@) ==> forall|candidate: OutRow| #![trigger top_covers(final(rows)@, candidate)]
            top_covers(old(rows)@, candidate) ==> top_covers(final(rows)@, candidate),
        output_sorted(old(rows)@) ==> top_dominates(final(rows)@, row),
        output_sorted(old(rows)@) ==> forall|candidate: WRow| #![trigger top_dominates(final(rows)@, candidate)]
            top_dominates(old(rows)@, candidate) ==> top_dominates(final(rows)@, candidate),
        forall|position: int| #![trigger final(rows)@[position]] 0 <= position < final(rows)@.len() ==>
            final(rows)@[position] == row || old(rows)@.contains(final(rows)@[position]),
        output_sorted(old(rows)@) ==> output_sorted(final(rows)@),
        output_distinct(old(rows)@) && (forall|position: int| #![trigger old(rows)@[position]] 0 <= position < old(rows)@.len() ==>
            !same_group(output_row(old(rows)@[position]), output_row(row))) ==> output_distinct(final(rows)@),
{
    let ghost original = rows@;
    let mut low = 0;
    let mut high = rows.len();
    while low < high
        invariant low <= high <= rows.len(), rows.len() <= 10, rows@ == original,
            output_sorted(original) ==> forall|position: int| #![trigger rows@[position]] 0 <= position < low ==> ext_lt(output_row(rows@[position]), output_row(row)),
            output_sorted(original) ==> forall|position: int| #![trigger rows@[position]] high <= position < rows@.len() ==> !ext_lt(output_row(rows@[position]), output_row(row)),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if before(rows[middle], row) {
            proof {
                if output_sorted(original) {
                    assert forall|position: int| #![trigger rows@[position]] 0 <= position <= middle implies ext_lt(output_row(rows@[position]), output_row(row)) by {
                        if position < middle { assert(!ext_lt(output_row(rows@[middle as int]), output_row(rows@[position]))); }
                    }
                }
            }
            low = middle + 1;
        } else {
            proof {
                if output_sorted(original) {
                    assert forall|position: int| #![trigger rows@[position]] middle <= position < rows@.len() implies !ext_lt(output_row(rows@[position]), output_row(row)) by {
                        if position > middle { assert(!ext_lt(output_row(rows@[position]), output_row(rows@[middle as int]))); }
                    }
                }
            }
            high = middle;
        }
    }
    if low >= 10 {
        proof {
            if output_sorted(original) {
                assert(ext_lt(output_row(original[9]), output_row(row)));
                assert(top_dominates(rows@, row));
            }
        }
        return;
    }
    let ghost previous = rows@;
    proof {
        if output_sorted(original) {
            let less = |left: WRow, right: WRow| !ext_lt(output_row(right), output_row(left));
            assert forall|position: int| #![trigger previous[position]] 0 <= position < low implies less(previous[position], row) by {}
            verus_prooflib::lemma_sorted_insert_at(previous, low as int, row, less);
        }
    }
    rows.insert(low, row);
    proof {
        assert forall|position: int| #![trigger rows@[position]] 0 <= position < rows@.len() implies
            rows@[position] == row || previous.contains(rows@[position]) by {
            if position != low {
                let prior = if position < low { position } else { position - 1 };
                assert(rows@[position] == previous[prior]);
                assert(previous.contains(previous[prior]));
            }
        }
        if output_distinct(original) && (forall|position: int| #![trigger original[position]] 0 <= position < original.len() ==> !same_group(output_row(original[position]), output_row(row))) {
            assert forall|left: int, right: int| #![trigger rows@[left], rows@[right]] 0 <= left < right < rows@.len() implies !same_group(output_row(rows@[left]), output_row(rows@[right])) by {
                if left == low { assert(rows@[right] == original[right - 1]); }
                else if right == low { assert(rows@[left] == original[left]); }
                else {
                    let old_left = if left < low { left } else { left - 1 };
                    let old_right = if right < low { right } else { right - 1 };
                    assert(rows@[left] == original[old_left]);
                    assert(rows@[right] == original[old_right]);
                }
            }
        }
    }
    if rows.len() > 10 { rows.pop(); }
    proof {
        if output_sorted(original) {
            assert(rows@ == if original.len() < 10 { original.insert(low as int, row) } else { original.insert(low as int, row).take(10) });
            lemma_top_insert(original, rows@, row, low as int);
            lemma_retention_cover(original, rows@);
            if rows@.contains(row) {
                let position = choose|position: int| #![trigger rows@[position]] 0 <= position < rows@.len() && rows@[position] == row;
            }
        }
    }
}

pub fn evaluate(state: &State, segment: &[u8], date: i32, id: GroupId) -> (result: Option<Option<WRow>>)
    requires id.date < date,
    ensures result matches Some(Some(row)) ==> row_ok(state.db(), params(segment@, date as int), output_row(row)),
        result matches Some(Some(row)) ==> row.0 == id.key && row.2 == id.date && row.3 == id.priority,
        result matches Some(candidate) ==> candidate.is_some() == (group_count(state.db(), params(segment@, date as int), id.key as int, id.date as int, id.priority as int) > 0),
{
    state.facts();
    let page = match state.pages.get(&Key::number(page_id(id.key))) { Some(page) => page, None => return None };
    let position = group_start_packed(&page.groups, segment, id);
    if position >= page.groups.len() { return None; }
    let group_value = page.groups.get(position);
    let group = &group_value;
    if group.id.key != id.key || group.id.date != id.date || group.id.priority != id.priority
        || !bytes_eq_exec(segment, &group.segment) { return None; }
    let mut count: Wide = wide::zero();
    let mut revenue: Wide = wide::zero();
    let mut ship_index = ships_start_packed(&page.ships, id.key);
    proof {
        let prefix = (*page.ships)@.take(ship_index as int);
        lemma_sum_zero(prefix, shipment_weight(id.key as int, date as int, 0));
        lemma_sum_zero(prefix, shipment_weight(id.key as int, date as int, 1));
    }
    while ship_index < page.ships.len() && page.ships.get(ship_index).key == id.key
        invariant ship_index <= (*page.ships)@.len(), ships_sorted((*page.ships)@),
            forall|position: int| #![trigger (*page.ships)@[position]] 0 <= position < (*page.ships)@.len() ==> ship_ok((*page.ships)@[position]),
            0 <= count.int() <= ship_index as int * 0x7FFF_FFFF_FFFF_FFFFint,
            -(ship_index as int * (0x8000_0000_0000_0000int * 0x8000_0000_0000_0000_0000_0000_0000_0000int)) <= revenue.int(),
            revenue.int() <= ship_index as int * (0x8000_0000_0000_0000int * 0x8000_0000_0000_0000_0000_0000_0000_0000int),
            count.int() == shipment_suffix((*page.ships)@.take(ship_index as int), id.key as int, date as int, 0),
            revenue.int() == shipment_suffix((*page.ships)@.take(ship_index as int), id.key as int, date as int, 1),
            forall|position: int| #![trigger (*page.ships)@[position]] ship_index <= position < (*page.ships)@.len() ==> (*page.ships)@[position].key >= id.key,
        decreases (*page.ships)@.len() - ship_index,
    {
        let ship = page.ships.get(ship_index);
        proof { assert(ship_ok((*page.ships)@[ship_index as int])); }
        if ship.date > date {
            count = wide::add(count, wide::from_i128(ship.count as i128));
            revenue = wide::add(revenue, ship.revenue);
        }
        proof {
            lemma_sum_take((*page.ships)@, ship_index as int,
                shipment_weight(id.key as int, date as int, 0));
            lemma_sum_take((*page.ships)@, ship_index as int,
                shipment_weight(id.key as int, date as int, 1));
        }
        ship_index += 1;
    }
    proof {
            assert forall|offset: int| #![trigger (*page.ships)@[offset]] ship_index <= offset < (*page.ships)@.len() implies (*page.ships)@[offset].key != id.key by {
                if ship_index < (*page.ships)@.len() {
                    assert((*page.ships)@[ship_index as int].key > id.key);
                    if offset > ship_index { assert(ship_lt((*page.ships)@[ship_index as int], (*page.ships)@[offset])); }
                }
            }
            verus_prooflib::lemma_sum_prefix_skip_zero((*page.ships)@, ship_index as int, (*page.ships)@.len() as int,
                shipment_weight(id.key as int, date as int, 0));
            verus_prooflib::lemma_sum_prefix_skip_zero((*page.ships)@, ship_index as int, (*page.ships)@.len() as int,
                shipment_weight(id.key as int, date as int, 1));
            assert((*page.ships)@.take((*page.ships)@.len() as int) =~= (*page.ships)@);
            lemma_shipment_suffix(state.customers, state.pages, state.ranks, state.db(), &*page, id.key as int / 256, id.key as int, date as int, 0);
            lemma_shipment_suffix(state.customers, state.pages, state.ranks, state.db(), &*page, id.key as int / 256, id.key as int, date as int, 1);
            lemma_group_weight(state.customers, state.pages, state.ranks, state.db(), &*page, id.key as int / 256, position as int);
            lemma_sql_group(state.db(), params(segment@, date as int), id.key as int, id.date as int, id.priority as int);
            assert(group.weight > 0);
    }
    if count.hi != 0 || count.lo != 0 {
        if group.weight >= 0x10_0000_0000_0000_0000_0000_0000 || revenue.hi < -0x20_0000 || revenue.hi > 0x20_0000 { return None; }
        let joined_revenue = wide::mul(revenue, group.weight as u128);
        proof {
            assert(count.int() > 0);
            assert(group.weight as int * count.int() > 0) by (nonlinear_arith)
                requires group.weight > 0, count.int() > 0;
            assert(row_ok(state.db(), params(segment@, date as int), output_row((id.key, joined_revenue, id.date, id.priority))));
        }
        Some(Some((id.key, joined_revenue, id.date, id.priority)))
    } else {
        proof {
            assert(count.int() == 0);
            assert(group_count(state.db(), params(segment@, date as int), id.key as int, id.date as int, id.priority as int) == 0) by (nonlinear_arith)
                requires count.int() == 0,
                    group_count(state.db(), params(segment@, date as int), id.key as int, id.date as int, id.priority as int) == group.weight as int * count.int();
        }
        Some(None)
    }
}

spec fn rows_read_ok(db: Db, segment: Seq<u8>, date: int, rows: Seq<WRow>, visited: IMap<(i64, i64, i64), ()>) -> bool {
    &&& rows.len() <= 10 && output_sorted(rows) && output_distinct(rows)
    &&& forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() ==>
        row_ok(db, params(segment, date), output_row(rows[position]))
            && visited.contains_key((rows[position].0, rows[position].2 as i64, rows[position].3))
    &&& forall|row: OutRow| #![trigger row_ok(db, params(segment, date), row)]
        row_ok(db, params(segment, date), row) && visited.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64)) ==> top_covers(rows, row)
}

fn visit(state: &State, segment: &[u8], date: i32, id: GroupId, visited: &mut IntegerTripleMap<()>, rows: &mut Vec<WRow>) -> (result: bool)
    requires rows_read_ok(state.db(), segment@, date as int, old(rows)@, old(visited)@),
        !old(visited)@.contains_key((id.key, id.date as i64, id.priority)),
    ensures final(visited)@ == old(visited)@.insert((id.key, id.date as i64, id.priority), ()),
        result ==> rows_read_ok(state.db(), segment@, date as int, final(rows)@, final(visited)@),
{
    state.facts();
    let ghost previous_visited = visited@;
    let ghost original_rows = rows@;
    let visited_key = (id.key, id.date as i64, id.priority);
    visited.insert(visited_key, ());
    let candidate = if id.date < date {
        match evaluate(state, segment, date, id) { Some(candidate) => candidate, None => return false }
    } else { None };
    if let Some(row) = candidate {
        proof {
            assert forall|position: int| #![trigger original_rows[position]] 0 <= position < original_rows.len() implies
                !same_group(output_row(original_rows[position]), output_row(row)) by {
                assert(previous_visited.contains_key((original_rows[position].0, original_rows[position].2 as i64, original_rows[position].3)));
            }
        }
        retain(rows, row);
        proof {
            assert forall|position: int| #![trigger rows@[position]] 0 <= position < rows@.len() implies
                row_ok(state.db(), params(segment@, date as int), output_row(rows@[position]))
                    && visited@.contains_key((rows@[position].0, rows@[position].2 as i64, rows@[position].3)) by {
                if rows@[position] != row {
                    let prior = choose|prior: int| #![trigger original_rows[prior]] 0 <= prior < original_rows.len() && original_rows[prior] == rows@[position];
                }
            }
        }
    }
    proof {
        assert forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
            row_ok(state.db(), params(segment@, date as int), row) && visited@.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64))
                implies top_covers(rows@, row) by {
            if previous_visited.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64)) {
                assert(top_covers(original_rows, row));
            } else {
                lemma_candidate_info(state.customers, state.pages, state.ranks, state.db(), segment@, date, row);
                let group = selected_group(state.pages, segment@, row);
                assert((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64) == visited_key);
                assert(row.l_orderkey == id.key && row.o_orderdate == id.date && row.o_shippriority == id.priority);
                if id.date >= date {
                    lemma_sql_group(state.db(), params(segment@, date as int), id.key as int, id.date as int, id.priority as int);
                } else {
                    assert(candidate.is_some());
                    lemma_row_unique(state.db(), params(segment@, date as int), row, output_row(candidate.unwrap()));
                    assert(top_covers(rows@, output_row(candidate.unwrap())));
                }
            }
        }
    }
    true
}

pub fn read(state: &State, segment: &[u8], date: i32) -> (result: Option<Vec<WRow>>)
    ensures result matches Some(rows) ==> query_result(state.db(), params(segment@, date as int), wide_out(rows@)),
{
    state.facts();
    let mut streams = Vec::new();
    let mut offset = leaf(date);
    let mut level: i64 = 0;
    while level <= 26
        invariant 0 <= level <= 27, 0 <= offset < 67108864,
            offset as int == ((date as int - i32::MIN as int) / 128) / pow2(level as int),
            state_ok(state.customers, state.pages, state.ranks, state.db()),
            forall|position: int| #![trigger streams@[position]] 0 <= position < streams@.len() ==> stream_ordered(streams@[position]),
            forall|ancestor: int, rank: KeyView| #![trigger state.ranks.at((ancestor_node(date as int, ancestor), segment@)), pending(streams@, rank)]
                0 <= ancestor < level && state.ranks.at((ancestor_node(date as int, ancestor), segment@)).is_some()
                    && state.ranks.at((ancestor_node(date as int, ancestor), segment@)).unwrap().at(rank).is_some() ==> pending(streams@, rank),
        decreases 27 - level,
    {
        let ghost previous_streams = streams@;
        let key = Key::composite(level * 67108864 + offset, copy_bytes(segment));
        if let Some(index) = state.ranks.get(&key) {
            if let Some(stream) = open_stream(&index) {
                let ghost added = stream;
                streams.push(stream);
                proof {
                    assert forall|rank: KeyView| #![trigger pending(streams@, rank)]
                        pending(streams@, rank) == (pending(previous_streams, rank) || stream_has(added, rank)) by {
                        lemma_pending_push(previous_streams, added, rank);
                    }
                }
            }
        }
        proof {
            verus_prooflib::lemma_dyadic_width_bounds(level as int);
            vstd::arithmetic::div_mod::lemma_div_denominator((date as int - i32::MIN as int) / 128, pow2(level as int), 2);
        }
        offset = offset / 2;
        level += 1;
    }
    let mut visited: IntegerTripleMap<()> = IntegerTripleMap::new();
    let mut result: Vec<WRow> = Vec::new();
    proof {
        lemma_sum_nonneg(streams@, |entry: Stream| entry.cursor.remaining().len() as int + 1);
        assert forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
            row_ok(state.db(), params(segment@, date as int), row) implies pending(streams@, group_rank(selected_group(state.pages, segment@, row))) by {
            lemma_candidate_info(state.customers, state.pages, state.ranks, state.db(), segment@, date, row);
            let group = selected_group(state.pages, segment@, row);
            let ancestor = choose|ancestor: int| #![trigger listed(state.ranks, ancestor_node(date as int, ancestor), segment@, group)]
                0 <= ancestor <= 26 && listed(state.ranks, ancestor_node(date as int, ancestor), segment@, group);
            assert(state.ranks.at((ancestor_node(date as int, ancestor), segment@)).unwrap().at(group_rank(group)).is_some());
        }
    }
    while streams.len() > 0
        invariant result.len() <= 10, remaining(streams@) >= 0, output_sorted(result@), output_distinct(result@),
            state_ok(state.customers, state.pages, state.ranks, state.db()),
            forall|position: int| #![trigger streams@[position]] 0 <= position < streams@.len() ==> stream_ordered(streams@[position]),
            forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
                row_ok(state.db(), params(segment@, date as int), row) && visited@.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64))
                    ==> top_covers(result@, row),
            forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
                row_ok(state.db(), params(segment@, date as int), row) && !visited@.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64))
                    ==> pending(streams@, group_rank(selected_group(state.pages, segment@, row))),
            forall|position: int| #![trigger result@[position]] 0 <= position < result@.len() ==>
                row_ok(state.db(), params(segment@, date as int), output_row(result@[position])),
            forall|position: int| #![trigger result@[position]] 0 <= position < result@.len() ==>
                visited@.contains_key((result@[position].0, result@[position].2 as i64, result@[position].3)),
        decreases remaining(streams@),
    {
        let ghost previous_streams = streams@;
        let mut best = 0;
        let mut index = 1;
        while index < streams.len()
            invariant index <= streams.len(), best < streams.len(),
                forall|position: int| #![trigger streams@[position]] 0 <= position < index ==> streams@[best as int].head.upper >= streams@[position].head.upper,
            decreases streams.len() - index,
        {
            if streams[index].head.upper > streams[best].head.upper { best = index; }
            index += 1;
        }
        let bound = streams[best].head.upper as i128;
        if result.len() == 10 && bound < i64::MAX as i128 && wide::lt(wide::from_i128(bound), result[9].1) {
            proof {
                assert forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
                    row_ok(state.db(), params(segment@, date as int), row) implies top_covers(result@, row) by {
                    if !visited@.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64)) {
                        lemma_candidate_info(state.customers, state.pages, state.ranks, state.db(), segment@, date, row);
                        let group = selected_group(state.pages, segment@, row);
                        let rank = group_rank(group);
                        let source = choose|source: int| #![trigger streams@[source]] 0 <= source < streams@.len() && stream_has(streams@[source], rank);
                        let chosen = streams@[source];
                        if chosen.head.kv() != rank {
                            let position = choose|position: int| #![trigger chosen.cursor.remaining()[position]]
                                0 <= position < chosen.cursor.remaining().len() && chosen.cursor.remaining()[position].kv() == rank;
                            assert(key_lt(chosen.head.kv(), rank));
                        }
                        lemma_upper_of(chosen.head, rank);
                        assert(streams@[best as int].head.upper >= chosen.head.upper);
                        assert(rank.0 == -1 - group.bound as int);
                        assert(row.revenue < result@[9].1.int());
                    }
                }
                lemma_top_result(state.db(), params(segment@, date as int), result@);
            }
            return Some(result);
        }
        let stream = streams.remove(best);
        let ghost previous_stream = stream;
        let ghost remaining_streams = streams@;
        proof {
            lemma_sum_remove(previous_streams, best as int, |entry: Stream| entry.cursor.remaining().len() as int + 1);
        }
        let id = stream.head.id;
        let visited_key = (id.key, id.date as i64, id.priority);
        if !visited.contains_key(&visited_key) {
            if !visit(state, segment, date, id, &mut visited, &mut result) { return None; }
        }
        let advanced = advance(stream);
        let ghost next_stream = advanced;
        if let Some(next) = advanced {
            let ghost next_value = next;
            streams.push(next);
            proof {
                lemma_sum_push(remaining_streams, next_value, |entry: Stream| entry.cursor.remaining().len() as int + 1);
            }
        }
        proof {
            assert forall|rank: KeyView| #![trigger pending(streams@, rank)]
                pending(previous_streams, rank) && rank != previous_stream.head.kv() implies pending(streams@, rank) by {
                lemma_pending_remove(previous_streams, best as int, rank);
                if next_stream.is_some() {
                    lemma_pending_push(remaining_streams, next_stream.unwrap(), rank);
                }
            }
            assert forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
                row_ok(state.db(), params(segment@, date as int), row) && !visited@.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64))
                    implies pending(streams@, group_rank(selected_group(state.pages, segment@, row))) by {
                lemma_candidate_info(state.customers, state.pages, state.ranks, state.db(), segment@, date, row);
                let group = selected_group(state.pages, segment@, row);
                let rank = group_rank(group);
                if rank == previous_stream.head.kv() {
                    lemma_group_identity(group.id, id);
                    assert(visited@.contains_key(visited_key));
                }
                assert(pending(previous_streams, rank));
                assert(pending(streams@, rank));
            }
            lemma_sum_nonneg(streams@, |entry: Stream| entry.cursor.remaining().len() as int + 1);
            assert(remaining(streams@) < remaining(previous_streams));
        }
    }
    proof {
        assert forall|row: OutRow| #![trigger row_ok(state.db(), params(segment@, date as int), row)]
            row_ok(state.db(), params(segment@, date as int), row) implies top_covers(result@, row) by {
            if !visited@.contains_key((row.l_orderkey as i64, row.o_orderdate as i64, row.o_shippriority as i64)) {
                assert(pending(streams@, group_rank(selected_group(state.pages, segment@, row))));
            }
        }
        lemma_top_result(state.db(), params(segment@, date as int), result@);
    }
    Some(result)
}

}
