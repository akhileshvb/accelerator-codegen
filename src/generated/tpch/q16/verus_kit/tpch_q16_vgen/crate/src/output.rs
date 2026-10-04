use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, Cursor};
use crate::model::*;
use crate::counts::*;
use crate::output_proofs::*;

verus! {

pub struct Stream {
    pub cursor: Cursor<Cell>,
    pub head: Arc<Cell>,
    pub token: Arc<usize>,
    pub filter: bool,
    pub finished: bool,
}

pub open spec fn visible(cell: Cell, excluded: Seq<u8>, filter: bool) -> bool {
    !filter || (*cell.group.brand)@ != excluded
}

pub open spec fn source_cell(cell: Cell, counts: Counts) -> bool {
    cell.group.valid() && cell.count > 0 && counts.at(cell.group.key()) == Some(cell)
}

pub open spec fn cursor_source(rows: Seq<(verus_prooflib::ordered::KeyView, Cell)>, counts: Counts) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> source_cell((#[trigger] rows[position]).1, counts)
}

pub open spec fn streams_source(streams: Seq<Vec<Stream>>, counts: Counts) -> bool {
    forall|position: int| 0 <= position < streams.len() ==> {
        let slot = #[trigger] streams[position];
        slot@.len() == 1 && source_cell(*slot@[0].head, counts)
            && cursor_source(slot@[0].cursor.remaining(), counts)
    }
}

pub open spec fn cells_source(cells: Seq<Arc<Cell>>, counts: Counts) -> bool {
    forall|position: int| 0 <= position < cells.len() ==> source_cell(*(#[trigger] cells[position]), counts)
}

pub open spec fn cell_selected(cell: Cell, excluded: Seq<u8>, sizes: Seq<i64>) -> bool {
    sizes.contains(cell.group.size) && (*cell.group.brand)@ != excluded
}

pub open spec fn cursor_selected(rows: Seq<(verus_prooflib::ordered::KeyView, Cell)>, excluded: Seq<u8>, sizes: Seq<i64>, filter: bool) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> {
        let cell = (#[trigger] rows[position]).1;
        sizes.contains(cell.group.size) && (filter || (*cell.group.brand)@ != excluded)
    }
}

pub open spec fn streams_selected(streams: Seq<Vec<Stream>>, excluded: Seq<u8>, sizes: Seq<i64>) -> bool {
    forall|position: int| 0 <= position < streams.len() ==> {
        let slot = #[trigger] streams[position];
        slot@.len() == 1 && cell_selected(*slot@[0].head, excluded, sizes)
            && cursor_selected(slot@[0].cursor.remaining(), excluded, sizes, slot@[0].filter)
    }
}

pub open spec fn cells_selected(cells: Seq<Arc<Cell>>, excluded: Seq<u8>, sizes: Seq<i64>) -> bool {
    forall|position: int| 0 <= position < cells.len() ==> cell_selected(*(#[trigger] cells[position]), excluded, sizes)
}

pub proof fn lemma_rank_source(rows: &Counts, counts: Counts, size: int, brand: Option<Seq<u8>>)
    requires positive_counts(counts), stream_valid(*rows, counts, size, brand),
    ensures cursor_source(rows.entries(), counts),
{
    rows.model();
    assert forall|position: int| 0 <= position < rows.entries().len() implies
        source_cell((#[trigger] rows.entries()[position]).1, counts) by {
        let entry = rows.entries()[position];
        assert(rows.at(entry.0) == Some(entry.1));
        assert(counts.at(entry.1.group.key()) == Some(entry.1));
    }
}

pub fn next_visible(cursor: &mut Cursor<Cell>, excluded: &[u8], filter: bool, Ghost(counts): Ghost<Counts>, Ghost(sizes): Ghost<Seq<i64>>) -> (out: Option<Arc<Cell>>)
    requires cursor_source(old(cursor).remaining(), counts),
        cursor_selected(old(cursor).remaining(), excluded@, sizes, filter),
    ensures cursor_source(final(cursor).remaining(), counts),
        cursor_selected(final(cursor).remaining(), excluded@, sizes, filter),
        out matches Some(cell) ==> cell_selected(*cell, excluded@, sizes),
        out matches Some(cell) ==> source_cell(*cell, counts),
        final(cursor).remaining().len() <= old(cursor).remaining().len(),
        out.is_some() ==> final(cursor).remaining().len() < old(cursor).remaining().len(),
        out matches Some(cell) ==> visible(*cell, excluded@, filter)
            && exists|position: int| 0 <= position < old(cursor).remaining().len()
                && (#[trigger] old(cursor).remaining()[position]).1 == *cell
                && final(cursor).remaining() == old(cursor).remaining().skip(position + 1)
                && forall|earlier: int| 0 <= earlier < position ==>
                    !visible((#[trigger] old(cursor).remaining()[earlier]).1, excluded@, filter),
        out.is_none() ==> final(cursor).remaining().len() == 0
            && forall|position: int| 0 <= position < old(cursor).remaining().len() ==>
                !visible((#[trigger] old(cursor).remaining()[position]).1, excluded@, filter),
{
    let initial = Ghost(cursor.remaining().len());
    let rows = Ghost(cursor.remaining());
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant initial@ == old(cursor).remaining().len(), cursor.remaining().len() <= initial@,
            cursor_source(rows@, counts), cursor_source(cursor.remaining(), counts),
            cursor_selected(rows@, excluded@, sizes, filter), cursor_selected(cursor.remaining(), excluded@, sizes, filter),
            rows@ == old(cursor).remaining(), 0 <= processed@ <= rows@.len(),
            cursor.remaining() == rows@.skip(processed@),
            forall|position: int| 0 <= position < processed@ ==>
                !visible((#[trigger] rows@[position]).1, excluded@, filter),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (_, cell) = match cursor.next() { Some(row) => row, None => return None };
        proof {
            assert(remaining@[0].1 == *cell);
            assert(rows@[processed@].1 == *cell);
            assert(cursor.remaining() =~= rows@.skip(processed@ + 1));
            assert forall|position: int| 0 <= position < cursor.remaining().len() implies
                source_cell((#[trigger] cursor.remaining()[position]).1, counts) by {
                assert(cursor.remaining()[position] == rows@[processed@ + 1 + position]);
            }
            assert forall|position: int| 0 <= position < cursor.remaining().len() implies {
                let cell = (#[trigger] cursor.remaining()[position]).1;
                sizes.contains(cell.group.size) && (filter || (*cell.group.brand)@ != excluded@)
            } by {
                assert(cursor.remaining()[position] == rows@[processed@ + 1 + position]);
            }
        }
        if !filter || !bytes_eq_exec(cell.group.brand.as_slice(), excluded) {
            proof {
                assert(exists|position: int| 0 <= position < rows@.len()
                    && (#[trigger] rows@[position]).1 == *cell
                    && cursor.remaining() == rows@.skip(position + 1)
                    && forall|earlier: int| 0 <= earlier < position ==>
                        !visible((#[trigger] rows@[earlier]).1, excluded@, filter));
            }
            return Some(cell);
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn open_stream(streams: &mut Vec<Vec<Stream>>, frontier: &mut Index<usize>,
    mut cursor: Cursor<Cell>, excluded: &[u8], filter: bool, Ghost(counts): Ghost<Counts>, Ghost(sizes): Ghost<Seq<i64>>,
) -> bool
    requires frontier.wf(),
        streams_source(streams@, counts), cursor_source(cursor.remaining(), counts),
        streams_selected(streams@, excluded@, sizes), cursor_selected(cursor.remaining(), excluded@, sizes, filter),
        forall|position: int| 0 <= position < streams@.len() ==> (#[trigger] streams@[position])@.len() == 1,
    ensures final(frontier).wf(),
        streams_source(final(streams)@, counts), streams_selected(final(streams)@, excluded@, sizes),
        forall|position: int| 0 <= position < final(streams)@.len() ==> (#[trigger] final(streams)@[position])@.len() == 1,
{
    let head = match next_visible(&mut cursor, excluded, filter, Ghost(counts), Ghost(sizes)) { Some(value) => value, None => return true };
    let token = Arc::new(streams.len());
    frontier.put(head.group.order.with_number(-head.count), token.clone());
    let mut slot = Vec::new();
    slot.push(Stream { cursor, head, token, filter, finished: false });
    streams.push(slot);
    true
}

pub fn head_lt(left: &Cell, right: &Cell) -> (less: bool)
    ensures less == crate::gen_spec::row_lt(
        crate::gen_spec::OutRow { p_brand: (*left.group.brand)@, p_type: (*left.group.kind)@,
            p_size: left.group.size as int, supplier_cnt: left.count as int },
        crate::gen_spec::OutRow { p_brand: (*right.group.brand)@, p_type: (*right.group.kind)@,
            p_size: right.group.size as int, supplier_cnt: right.count as int }),
{
    proof {
        lemma_bytes_lt_irrefl((*left.group.brand)@);
        lemma_bytes_lt_irrefl((*left.group.kind)@);
    }
    if left.count != right.count { return left.count > right.count; }
    if !bytes_eq_exec(left.group.brand.as_slice(), right.group.brand.as_slice()) {
        return bytes_lt_exec(left.group.brand.as_slice(), right.group.brand.as_slice());
    }
    if !bytes_eq_exec(left.group.kind.as_slice(), right.group.kind.as_slice()) {
        return bytes_lt_exec(left.group.kind.as_slice(), right.group.kind.as_slice());
    }
    left.group.size < right.group.size
}

pub open spec fn stream_work(slot: Vec<Stream>) -> int {
    if slot@.len() == 1 && !slot@[0].finished { slot@[0].cursor.remaining().len() as int + 1 }
    else { 0 }
}

pub open spec fn winners_valid(winners: Seq<Option<usize>>, length: int) -> bool {
    forall|position: int| 0 <= position < winners.len() ==>
        ((#[trigger] winners[position]) matches Some(index) ==> index < length)
}

pub fn choose_stream(streams: &Vec<Vec<Stream>>, left: Option<usize>, right: Option<usize>, Ghost(counts): Ghost<Counts>) -> (winner: Option<usize>)
    requires streams_source(streams@, counts),
        left matches Some(index) ==> index < streams.len(),
        right matches Some(index) ==> index < streams.len(),
    ensures winner matches Some(index) ==> index < streams.len(),
{
    match (left, right) {
        (None, other) => other,
        (other, None) => other,
        (Some(first), Some(second)) => {
            if head_lt(&*streams[second][0].head, &*streams[first][0].head) { Some(second) }
            else { Some(first) }
        },
    }
}

pub fn merge_small(mut streams: Vec<Vec<Stream>>, excluded: &[u8], Ghost(counts): Ghost<Counts>, Ghost(sizes): Ghost<Seq<i64>>) -> (result: Option<Vec<Arc<Cell>>>)
    requires streams.len() <= 8, streams_source(streams@, counts), streams_selected(streams@, excluded@, sizes),
    ensures result matches Some(cells) ==> cells_source(cells@, counts)
        && cells_selected(cells@, excluded@, sizes),
{
    let mut out = Vec::new();
    let mut width = 1;
    while width < streams.len()
        invariant streams.len() <= 8, width == 1 || width == 2 || width == 4 || width == 8,
        decreases 8 - width,
    {
        width = width * 2;
    }
    let mut winners: Vec<Option<usize>> = Vec::new();
    while winners.len() < width * 2
        invariant 1 <= width <= 8, streams.len() <= width, winners.len() <= width * 2,
            winners_valid(winners@, streams.len() as int),
        decreases width * 2 - winners.len(),
    {
        winners.push(None);
    }
    let mut index = 0;
    while index < streams.len()
        invariant index <= streams.len(), streams.len() <= width, 1 <= width <= 8,
            winners.len() == width * 2, winners_valid(winners@, streams.len() as int),
            streams_source(streams@, counts),
        decreases streams.len() - index,
    {
        if !streams[index][0].finished { winners.set(width + index, Some(index)); }
        index += 1;
    }
    let mut node = width;
    while node > 1
        invariant 1 <= node <= width, 1 <= width <= 8, winners.len() == width * 2,
            winners_valid(winners@, streams.len() as int), streams_source(streams@, counts),
        decreases node,
    {
        node -= 1;
        let winner = choose_stream(&streams, winners[node * 2], winners[node * 2 + 1], Ghost(counts));
        winners.set(node, winner);
    }
    proof { lemma_sum_nonneg(streams@, |slot: Vec<Stream>| stream_work(slot)); }
    loop
        invariant sum(streams@, |slot: Vec<Stream>| stream_work(slot)) >= 0,
            streams_source(streams@, counts), cells_source(out@, counts),
            streams_selected(streams@, excluded@, sizes), cells_selected(out@, excluded@, sizes),
            1 <= width <= 8, streams.len() <= width, winners.len() == width * 2,
            winners_valid(winners@, streams.len() as int),
        decreases sum(streams@, |slot: Vec<Stream>| stream_work(slot)),
    {
        let initial = Ghost(streams@);
        let selected = match winners[1] { Some(value) => value, None => return Some(out) };
        if streams[selected][0].finished { return None; }
        let mut stream = match streams[selected].pop() { Some(value) => value, None => return None };
        let previous_stream = Ghost(stream);
        out.push(stream.head.clone());
        match next_visible(&mut stream.cursor, excluded, stream.filter, Ghost(counts), Ghost(sizes)) {
            Some(head) => {
                stream.head = head;
            },
            None => stream.finished = true,
        }
        streams[selected].push(stream);
        proof {
            assert(streams@ =~= initial@.update(selected as int, streams@[selected as int]));
            assert(previous_stream@ == initial@[selected as int]@[0]);
            assert(stream_work(streams@[selected as int]) < stream_work(initial@[selected as int]));
            lemma_small_stream_update(initial@, streams@, selected as int);
            lemma_sum_ext_all(initial@, |slot: Vec<Stream>| if true { stream_work(slot) } else { remaining_work(slot) },
                |slot: Vec<Stream>| stream_work(slot));
            lemma_sum_ext_all(streams@, |slot: Vec<Stream>| if true { stream_work(slot) } else { remaining_work(slot) },
                |slot: Vec<Stream>| stream_work(slot));
            lemma_sum_nonneg(streams@, |slot: Vec<Stream>| stream_work(slot));
        }
        let mut node = width + selected;
        winners.set(node, if streams[selected][0].finished { None } else { Some(selected) });
        while node > 1
            invariant 1 <= node < width * 2, 1 <= width <= 8, winners.len() == width * 2,
                winners_valid(winners@, streams.len() as int), streams_source(streams@, counts),
            decreases node,
        {
            node = node / 2;
            let winner = choose_stream(&streams, winners[node * 2], winners[node * 2 + 1], Ghost(counts));
            winners.set(node, winner);
        }
    }
}

pub fn read(ranks: &Ranks, excluded: &[u8], sizes: &[i64], Ghost(counts): Ghost<Counts>) -> (result: Option<Vec<Arc<Cell>>>)
    requires sizes.len() <= 8, ranks_valid(*ranks, counts),
    ensures result matches Some(cells) ==> cells_source(cells@, counts) && cells_selected(cells@, excluded@, sizes@),
{
    let mut streams: Vec<Vec<Stream>> = Vec::new();
    let mut frontier: Index<usize> = Index::new();
    let excluded_key = Key::text(copy_bytes(excluded));
    let mut index = 0;
    while index < sizes.len()
        invariant index <= sizes.len(), frontier.wf(), ranks_valid(*ranks, counts), streams_source(streams@, counts),
            streams_selected(streams@, excluded@, sizes@),
            excluded_key.view() == (0int, excluded@),
            forall|position: int| 0 <= position < streams@.len() ==> (#[trigger] streams@[position])@.len() == 1,
        decreases sizes.len() - index,
    {
        let size = sizes[index];
        let mut earlier = 0;
        let mut duplicate = false;
        while earlier < index
            invariant earlier <= index, index < sizes.len(),
            decreases index - earlier,
        {
            if sizes[earlier] == size { duplicate = true; break; }
            earlier += 1;
        }
        index += 1;
        if duplicate { continue; }
        let size_rows = match ranks.get(&Key::number(size)) { Some(value) => value, None => continue };
        let excluded_groups = match size_rows.brands.get(&excluded_key) { Some(value) => value.groups, None => 0 };
        if excluded_groups < 0 || excluded_groups > size_rows.groups { return None; }
        let included_groups = size_rows.groups - excluded_groups;
        if included_groups == 0 { continue; }
        if excluded_groups <= included_groups {
            proof {
                lemma_rank_source(&size_rows.rows, counts, size as int, None);
                size_rows.rows.model();
                assert forall|position: int| 0 <= position < size_rows.rows.entries().len() implies {
                    let cell = (#[trigger] size_rows.rows.entries()[position]).1;
                    sizes@.contains(cell.group.size) && (excluded_groups > 0 || (*cell.group.brand)@ != excluded@)
                } by {
                    let entry = size_rows.rows.entries()[position];
                    assert(size_rows.rows.at(entry.0) == Some(entry.1));
                    assert(stream_valid(size_rows.rows, counts, size as int, None));
                    assert(stream_matches(*entry.1.group, size as int, None));
                    assert(counts.at(entry.1.group.key()) == Some(entry.1));
                    assert(sizes@[index as int - 1] == size);
                    if (*entry.1.group.brand)@ == excluded@ && excluded_groups == 0 {
                        assert(size_valid(*size_rows, counts, size as int));
                        assert(brands_valid(size_rows.brands, counts, size as int));
                        assert(size_rows.brands.at(excluded_key.view()).is_some());
                    }
                }
            }
            if !open_stream(&mut streams, &mut frontier, size_rows.rows.cursor(), excluded, excluded_groups > 0, Ghost(counts), Ghost(sizes@)) { return None; }
        } else {
            let mut brands = size_rows.brands.cursor();
            let mut processed: Ghost<int> = Ghost(0);
            loop
                invariant frontier.wf(), ranks_valid(*ranks, counts), streams_source(streams@, counts),
                    streams_selected(streams@, excluded@, sizes@), sizes@.contains(size),
                    size_valid(*size_rows, counts, size as int),
                    0 <= processed@ <= size_rows.brands.entries().len(),
                    brands.remaining() == size_rows.brands.entries().skip(processed@),
                    forall|position: int| 0 <= position < streams@.len() ==> (#[trigger] streams@[position])@.len() == 1,
                decreases brands.remaining().len(),
            {
                let (brand, rows) = match brands.next() { Some(row) => row, None => break };
                proof {
                    assert((brand.view(), *rows) == size_rows.brands.entries()[processed@]);
                    assert(brands.remaining() =~= size_rows.brands.entries().skip(processed@ + 1));
                    size_rows.brands.model();
                    assert(size_rows.brands.entries().contains((brand.view(), *rows)));
                    assert(size_rows.brands.at(brand.view()) == Some(*rows));
                    lemma_rank_source(&rows.rows, counts, size as int, Some(brand.view().1));
                }
                processed = Ghost(processed@ + 1);
                if bytes_eq_exec(brand.text.as_slice(), excluded) { continue; }
                proof {
                    rows.rows.model();
                    assert forall|position: int| 0 <= position < rows.rows.entries().len() implies {
                        let cell = (#[trigger] rows.rows.entries()[position]).1;
                        sizes@.contains(cell.group.size) && (*cell.group.brand)@ != excluded@
                    } by {
                        let entry = rows.rows.entries()[position];
                        assert(rows.rows.at(entry.0) == Some(entry.1));
                    }
                }
                if !open_stream(&mut streams, &mut frontier, rows.rows.cursor(), excluded, false, Ghost(counts), Ghost(sizes@)) { return None; }
            }
        }
    }
    if streams.len() <= 8 { return merge_small(streams, excluded, Ghost(counts), Ghost(sizes@)); }
    let mut out = Vec::new();
    proof { lemma_sum_nonneg(streams@, |slot: Vec<Stream>| remaining_work(slot)); }
    while !frontier.is_empty()
        invariant frontier.wf(), streams_source(streams@, counts), cells_source(out@, counts),
            streams_selected(streams@, excluded@, sizes@), cells_selected(out@, excluded@, sizes@),
            sum(streams@, |slot: Vec<Stream>| remaining_work(slot)) >= 0,
            forall|position: int| 0 <= position < streams@.len() ==> (#[trigger] streams@[position])@.len() == 1,
        decreases sum(streams@, |slot: Vec<Stream>| remaining_work(slot)) + frontier.entries().len(),
    {
        let initial_streams = Ghost(streams@);
        let initial_frontier = Ghost(frontier);
        proof { frontier.model(); }
        let mut cursor = frontier.cursor();
        let (key, token) = match cursor.next() { Some(row) => row, None => return None };
        proof {
            assert(frontier.at(frontier.entries()[0].0) == Some(frontier.entries()[0].1));
            assert(frontier.at(key.view()).is_some());
        }
        frontier.remove(&key);
        proof { lemma_frontier_remove(&initial_frontier@, &frontier, key.view()); }
        if *token >= streams.len() { return None; }
        let mut stream = match streams[*token].pop() { Some(value) => value, None => return None };
        let previous_stream = Ghost(stream);
        out.push(stream.head.clone());
        let next = next_visible(&mut stream.cursor, excluded, stream.filter, Ghost(counts), Ghost(sizes@));
        let advanced = Ghost(next.is_some());
        if let Some(head) = next {
            let before_put = Ghost(frontier);
            let next_key = head.group.order.with_number(-head.count);
            let next_view = Ghost(next_key.view());
            frontier.put(next_key, stream.token.clone());
            proof { lemma_frontier_put(&before_put@, &frontier, next_view@, *stream.token); }
            stream.head = head;
        }
        streams[*token].push(stream);
        proof {
            assert(streams@ =~= initial_streams@.update(*token as int, streams@[*token as int]));
            assert(previous_stream@ == initial_streams@[*token as int]@[0]);
            if advanced@ {
                assert(remaining_work(streams@[*token as int]) < remaining_work(initial_streams@[*token as int]));
                assert(frontier.entries().len() <= initial_frontier@.entries().len());
            } else {
                assert(remaining_work(streams@[*token as int]) <= remaining_work(initial_streams@[*token as int]));
                assert(frontier.entries().len() as int == initial_frontier@.entries().len() as int - 1);
            }
            lemma_large_stream_update(initial_streams@, streams@, *token as int);
            lemma_sum_ext_all(initial_streams@, |slot: Vec<Stream>| if false { stream_work(slot) } else { remaining_work(slot) },
                |slot: Vec<Stream>| remaining_work(slot));
            lemma_sum_ext_all(streams@, |slot: Vec<Stream>| if false { stream_work(slot) } else { remaining_work(slot) },
                |slot: Vec<Stream>| remaining_work(slot));
            assert(sum(streams@, |slot: Vec<Stream>| remaining_work(slot)) + frontier.entries().len()
                < sum(initial_streams@, |slot: Vec<Stream>| remaining_work(slot)) + initial_frontier@.entries().len());
            lemma_sum_nonneg(streams@, |slot: Vec<Stream>| remaining_work(slot));
        }
    }
    Some(out)
}

}
