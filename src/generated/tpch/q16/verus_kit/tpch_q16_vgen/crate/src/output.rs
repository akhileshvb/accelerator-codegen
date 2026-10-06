use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, Cursor, KeyView};
use verus_prooflib::like_match_exec;
use verus_common::sqlfns::like_match;
use crate::model::*;
use crate::counts::*;
use crate::vocab::*;
use crate::output_proofs::*;

verus! {

pub struct Stream {
    pub cursor: Cursor<Cell>,
    pub head: Arc<Cell>,
    pub finished: bool,
    pub orig: Ghost<Seq<(KeyView, Cell)>>,
}

pub struct Slot {
    pub kind: Arc<Vec<u8>>,
    pub hit: bool,
}

/// A direct-mapped cache of `like_match(kind, pattern)` by type string: the
/// pattern is evaluated once per distinct type (at most a few hundred) and
/// every later check of the same type is a hash plus a byte compare.
pub struct LikeMemo {
    pub slots: Vec<Option<Slot>>,
}

impl LikeMemo {
    pub open spec fn ok(&self, pattern: Seq<u8>) -> bool {
        self.slots@.len() == 1024 && forall|index: int| 0 <= index < 1024 ==>
            match #[trigger] self.slots@[index] {
                Some(slot) => slot.hit == like_match((*slot.kind)@, pattern),
                None => true,
            }
    }
}

pub fn new_memo(Ghost(pattern): Ghost<Seq<u8>>) -> (memo: LikeMemo)
    ensures memo.ok(pattern),
{
    let mut slots: Vec<Option<Slot>> = Vec::new();
    while slots.len() < 1024
        invariant slots.len() <= 1024,
            forall|index: int| 0 <= index < slots@.len() ==> (#[trigger] slots@[index]).is_none(),
        decreases 1024 - slots.len(),
    {
        slots.push(None);
    }
    LikeMemo { slots }
}

pub fn kind_slot(kind: &[u8]) -> (slot: usize)
    ensures slot < 1024,
{
    let mut hash: u64 = 0;
    let mut index = 0;
    while index < kind.len()
        invariant index <= kind.len(), hash < 65521,
        decreases kind.len() - index,
    {
        hash = (hash * 33 + kind[index] as u64) % 65521;
        index += 1;
    }
    (hash % 1024) as usize
}

pub fn memo_match(memo: &mut LikeMemo, kind: &Arc<Vec<u8>>, pattern: &[u8]) -> (hit: bool)
    requires old(memo).ok(pattern@),
    ensures final(memo).ok(pattern@), hit == like_match((**kind)@, pattern@),
{
    let slot = kind_slot(kind.as_slice());
    if let Some(entry) = &memo.slots[slot] {
        if bytes_eq_exec(entry.kind.as_slice(), kind.as_slice()) { return entry.hit; }
    }
    let hit = like_match_exec(kind.as_slice(), pattern);
    memo.slots.set(slot, Some(Slot { kind: kind.clone(), hit }));
    hit
}


/// What a read needs to decide visibility without touching strings: the
/// NOT LIKE verdict of each vocabulary type, the code of the excluded brand,
/// and a string memo for the (never expected) types outside the vocabulary.
pub struct Vis {
    pub bits: Vec<bool>,
    pub excluded_id: u16,
    pub memo: LikeMemo,
}

impl Vis {
    pub open spec fn ok(&self, excluded: Seq<u8>, pattern: Seq<u8>) -> bool {
        self.bits@.len() == 150
        && (forall|i: int| 0 <= i < 150 ==> (#[trigger] self.bits@[i]) == !like_match(vocab_type(i), pattern))
        && self.excluded_id as int == brand_code(excluded)
        && self.memo.ok(pattern)
    }
}

pub fn new_vis(vocab: &Vocab, excluded: &[u8], pattern: &[u8]) -> (vis: Vis)
    requires vocab.ok(),
    ensures vis.ok(excluded@, pattern@),
{
    let mut bits: Vec<bool> = Vec::new();
    while bits.len() < 150
        invariant bits.len() <= 150, vocab.ok(),
            forall|j: int| 0 <= j < bits@.len() ==> (#[trigger] bits@[j]) == !like_match(vocab_type(j), pattern@),
        decreases 150 - bits.len(),
    {
        let i = bits.len();
        let matched = like_match_exec(vocab.types[i].as_slice(), pattern);
        bits.push(!matched);
    }
    Vis { bits, excluded_id: brand_code_exec(vocab, excluded), memo: new_memo(Ghost(pattern@)) }
}

pub fn cell_visible(vis: &mut Vis, cell: &Cell, excluded: &[u8], pattern: &[u8]) -> (visible: bool)
    requires old(vis).ok(excluded@, pattern@), cell.group.valid(), cell.consistent(),
    ensures final(vis).ok(excluded@, pattern@),
        visible == ((*cell.group.brand)@ != excluded@ && !like_match((*cell.group.kind)@, pattern@)),
{
    proof {
        assert(cell.brand_id == cell.group.brand_id && cell.kind_id == cell.group.kind_id);
        lemma_brand_code_sound((*cell.group.brand)@);
        lemma_brand_code_sound(excluded@);
        lemma_type_code_sound((*cell.group.kind)@);
    }
    let brand_ok = if cell.brand_id < 25 { cell.brand_id != vis.excluded_id }
        else { !bytes_eq_exec(cell.group.brand.as_slice(), excluded) };
    if !brand_ok { return false; }
    if cell.kind_id < 150 { vis.bits[cell.kind_id as usize] }
    else { !memo_match(&mut vis.memo, &cell.group.kind, pattern) }
}

pub fn brand_bytes<'a>(vocab: &'a Vocab, cell: &'a Cell) -> (bytes: &'a [u8])
    requires vocab.ok(), cell.group.valid(), cell.consistent(),
    ensures bytes@ == (*cell.group.brand)@,
{
    proof { assert(cell.brand_id == cell.group.brand_id); lemma_brand_code_sound((*cell.group.brand)@); }
    if cell.brand_id < 25 { vocab.brands[cell.brand_id as usize].as_slice() } else { cell.group.brand.as_slice() }
}

pub fn kind_bytes<'a>(vocab: &'a Vocab, cell: &'a Cell) -> (bytes: &'a [u8])
    requires vocab.ok(), cell.group.valid(), cell.consistent(),
    ensures bytes@ == (*cell.group.kind)@,
{
    proof { assert(cell.kind_id == cell.group.kind_id); lemma_type_code_sound((*cell.group.kind)@); }
    if cell.kind_id < 150 { vocab.types[cell.kind_id as usize].as_slice() } else { cell.group.kind.as_slice() }
}

pub open spec fn visible(cell: Cell, excluded: Seq<u8>, pattern: Seq<u8>) -> bool {
    (*cell.group.brand)@ != excluded && !like_match((*cell.group.kind)@, pattern)
}

pub open spec fn source_cell(cell: Cell, counts: Counts) -> bool {
    cell.group.valid() && cell.count > 0 && cell.consistent() && counts.at(cell.group.key()) == Some(cell)
}

pub open spec fn cursor_source(rows: Seq<(KeyView, Cell)>, counts: Counts) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> source_cell((#[trigger] rows[position]).1, counts)
}

pub open spec fn cells_source(cells: Seq<Arc<Cell>>, counts: Counts) -> bool {
    forall|position: int| 0 <= position < cells.len() ==> source_cell(*(#[trigger] cells[position]), counts)
}

pub open spec fn cell_selected(cell: Cell, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>) -> bool {
    sizes.contains(cell.group.size) && visible(cell, excluded, pattern)
}

pub open spec fn cursor_selected(rows: Seq<(KeyView, Cell)>, sizes: Seq<i64>) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> sizes.contains((#[trigger] rows[position]).1.group.size)
}

pub open spec fn cells_selected(cells: Seq<Arc<Cell>>, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>) -> bool {
    forall|position: int| 0 <= position < cells.len() ==> cell_selected(*(#[trigger] cells[position]), excluded, pattern, sizes)
}

pub open spec fn cell_has(out: Seq<Arc<Cell>>, cell: Cell) -> bool {
    exists|index: int| 0 <= index < out.len() && *(#[trigger] out[index]) == cell
}

pub open spec fn cells_cover(cells: Seq<Arc<Cell>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>) -> bool {
    forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && sizes.contains(counts.at(key).unwrap().group.size)
        && visible(counts.at(key).unwrap(), excluded, pattern) ==> cell_has(cells, counts.at(key).unwrap())
}

/// One stream per size: the cursor walks the size's rank order; every
/// entry the cursor has passed is either invisible, already emitted, or the
/// pending head.
pub open spec fn stream_state(s: Stream, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>, out: Seq<Arc<Cell>>) -> bool {
    &&& s.cursor.remaining().len() <= s.orig@.len()
    &&& s.cursor.remaining() == s.orig@.skip(s.orig@.len() - s.cursor.remaining().len())
    &&& cursor_source(s.orig@, counts)
    &&& cursor_selected(s.orig@, sizes)
    &&& source_cell(*s.head, counts)
    &&& (s.finished ==> s.cursor.remaining().len() == 0)
    &&& (!s.finished ==> 1 <= s.orig@.len() - s.cursor.remaining().len()
        && s.orig@[s.orig@.len() - s.cursor.remaining().len() - 1].1 == *s.head
        && visible(*s.head, excluded, pattern))
    &&& forall|pos: int| 0 <= pos < (if s.finished { s.orig@.len() - s.cursor.remaining().len() }
            else { s.orig@.len() - s.cursor.remaining().len() - 1 })
        && visible((#[trigger] s.orig@[pos]).1, excluded, pattern) ==> cell_has(out, s.orig@[pos].1)
}

pub open spec fn streams_state(streams: Seq<Vec<Stream>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>, out: Seq<Arc<Cell>>) -> bool {
    forall|j: int| 0 <= j < streams.len() ==> {
        let slot = #[trigger] streams[j];
        slot@.len() == 1 && stream_state(slot@[0], counts, excluded, pattern, sizes, out)
    }
}

pub open spec fn out_extends(old: Seq<Arc<Cell>>, new: Seq<Arc<Cell>>) -> bool {
    forall|cell: Cell| cell_has(old, cell) ==> cell_has(new, cell)
}

pub proof fn lemma_push_extends(old: Seq<Arc<Cell>>, new: Seq<Arc<Cell>>, item: Arc<Cell>)
    requires new =~= old.push(item),
    ensures out_extends(old, new), cell_has(new, *item),
{
    assert forall|cell: Cell| cell_has(old, cell) implies cell_has(new, cell) by {
        let index = choose|index: int| 0 <= index < old.len() && *(#[trigger] old[index]) == cell;
        assert(*new[index] == cell);
    }
    assert(*new[old.len() as int] == *item);
}

pub proof fn lemma_state_extends(s: Stream, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>, old: Seq<Arc<Cell>>, new: Seq<Arc<Cell>>)
    requires stream_state(s, counts, excluded, pattern, sizes, old), out_extends(old, new),
    ensures stream_state(s, counts, excluded, pattern, sizes, new),
{
    assert forall|pos: int| 0 <= pos < (if s.finished { s.orig@.len() - s.cursor.remaining().len() }
            else { s.orig@.len() - s.cursor.remaining().len() - 1 })
        && visible((#[trigger] s.orig@[pos]).1, excluded, pattern) implies cell_has(new, s.orig@[pos].1) by {
        assert(cell_has(old, s.orig@[pos].1));
    }
}

pub proof fn lemma_advance_some(p: Stream, s: Stream, position: int, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>,
    before_out: Seq<Arc<Cell>>, out: Seq<Arc<Cell>>)
    requires stream_state(p, counts, excluded, pattern, sizes, before_out), !p.finished,
        out_extends(before_out, out), cell_has(out, *p.head),
        s.orig@ == p.orig@, !s.finished,
        0 <= position < p.cursor.remaining().len(),
        p.cursor.remaining()[position].1 == *s.head,
        s.cursor.remaining() == p.cursor.remaining().skip(position + 1),
        forall|e: int| 0 <= e < position ==> !visible((#[trigger] p.cursor.remaining()[e]).1, excluded, pattern),
        visible(*s.head, excluded, pattern), source_cell(*s.head, counts),
    ensures stream_state(s, counts, excluded, pattern, sizes, out),
{
    let k = p.orig@.len() - p.cursor.remaining().len();
    let k2 = s.orig@.len() - s.cursor.remaining().len();
    assert(k2 == k + position + 1);
    assert(s.cursor.remaining() =~= s.orig@.skip(k2));
    assert(p.cursor.remaining()[position] == p.orig@[k + position]);
    assert forall|pos: int| 0 <= pos < k2 - 1 && visible((#[trigger] s.orig@[pos]).1, excluded, pattern)
        implies cell_has(out, s.orig@[pos].1) by {
        if pos < k - 1 { assert(cell_has(before_out, p.orig@[pos].1)); }
        else if pos == k - 1 { assert(p.orig@[pos].1 == *p.head); }
        else {
            assert(p.cursor.remaining()[pos - k] == p.orig@[pos]);
            assert(!visible(p.cursor.remaining()[pos - k].1, excluded, pattern));
        }
    }
}

pub proof fn lemma_advance_none(p: Stream, s: Stream, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>,
    before_out: Seq<Arc<Cell>>, out: Seq<Arc<Cell>>)
    requires stream_state(p, counts, excluded, pattern, sizes, before_out), !p.finished,
        out_extends(before_out, out), cell_has(out, *p.head),
        s.orig@ == p.orig@, s.finished, s.head == p.head,
        s.cursor.remaining().len() == 0,
        forall|e: int| 0 <= e < p.cursor.remaining().len() ==> !visible((#[trigger] p.cursor.remaining()[e]).1, excluded, pattern),
    ensures stream_state(s, counts, excluded, pattern, sizes, out),
{
    let k = p.orig@.len() - p.cursor.remaining().len();
    assert(s.cursor.remaining() =~= s.orig@.skip(s.orig@.len() as int));
    assert forall|pos: int| 0 <= pos < s.orig@.len() && visible((#[trigger] s.orig@[pos]).1, excluded, pattern)
        implies cell_has(out, s.orig@[pos].1) by {
        if pos < k - 1 { assert(cell_has(before_out, p.orig@[pos].1)); }
        else if pos == k - 1 { assert(p.orig@[pos].1 == *p.head); }
        else {
            assert(p.cursor.remaining()[pos - k] == p.orig@[pos]);
            assert(!visible(p.cursor.remaining()[pos - k].1, excluded, pattern));
        }
    }
}

pub proof fn lemma_suffix_source(s: Stream, counts: Counts, sizes: Seq<i64>)
    requires s.cursor.remaining().len() <= s.orig@.len(),
        s.cursor.remaining() == s.orig@.skip(s.orig@.len() - s.cursor.remaining().len()),
        cursor_source(s.orig@, counts), cursor_selected(s.orig@, sizes),
    ensures cursor_source(s.cursor.remaining(), counts), cursor_selected(s.cursor.remaining(), sizes),
{
    let k = s.orig@.len() - s.cursor.remaining().len();
    assert forall|position: int| 0 <= position < s.cursor.remaining().len() implies
        source_cell((#[trigger] s.cursor.remaining()[position]).1, counts) by {
        assert(s.cursor.remaining()[position] == s.orig@[k + position]);
    }
    assert forall|position: int| 0 <= position < s.cursor.remaining().len() implies
        sizes.contains((#[trigger] s.cursor.remaining()[position]).1.group.size) by {
        assert(s.cursor.remaining()[position] == s.orig@[k + position]);
    }
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

pub fn next_visible(cursor: &mut Cursor<Cell>, vis: &mut Vis, excluded: &[u8], pattern: &[u8], Ghost(counts): Ghost<Counts>, Ghost(sizes): Ghost<Seq<i64>>) -> (out: Option<Arc<Cell>>)
    requires cursor_source(old(cursor).remaining(), counts),
        cursor_selected(old(cursor).remaining(), sizes),
        old(vis).ok(excluded@, pattern@),
    ensures final(vis).ok(excluded@, pattern@), cursor_source(final(cursor).remaining(), counts),
        cursor_selected(final(cursor).remaining(), sizes),
        out matches Some(cell) ==> cell_selected(*cell, excluded@, pattern@, sizes),
        out matches Some(cell) ==> source_cell(*cell, counts),
        final(cursor).remaining().len() <= old(cursor).remaining().len(),
        out.is_some() ==> final(cursor).remaining().len() < old(cursor).remaining().len(),
        out matches Some(cell) ==> visible(*cell, excluded@, pattern@)
            && exists|position: int| 0 <= position < old(cursor).remaining().len()
                && (#[trigger] old(cursor).remaining()[position]).1 == *cell
                && final(cursor).remaining() == old(cursor).remaining().skip(position + 1)
                && forall|earlier: int| 0 <= earlier < position ==>
                    !visible((#[trigger] old(cursor).remaining()[earlier]).1, excluded@, pattern@),
        out.is_none() ==> final(cursor).remaining().len() == 0
            && forall|position: int| 0 <= position < old(cursor).remaining().len() ==>
                !visible((#[trigger] old(cursor).remaining()[position]).1, excluded@, pattern@),
{
    let initial = Ghost(cursor.remaining().len());
    let rows = Ghost(cursor.remaining());
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant initial@ == old(cursor).remaining().len(), cursor.remaining().len() <= initial@, vis.ok(excluded@, pattern@),
            cursor_source(rows@, counts), cursor_source(cursor.remaining(), counts),
            cursor_selected(rows@, sizes), cursor_selected(cursor.remaining(), sizes),
            rows@ == old(cursor).remaining(), 0 <= processed@ <= rows@.len(),
            cursor.remaining() == rows@.skip(processed@),
            forall|position: int| 0 <= position < processed@ ==>
                !visible((#[trigger] rows@[position]).1, excluded@, pattern@),
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
            assert forall|position: int| 0 <= position < cursor.remaining().len() implies
                sizes.contains((#[trigger] cursor.remaining()[position]).1.group.size) by {
                assert(cursor.remaining()[position] == rows@[processed@ + 1 + position]);
            }
            assert(source_cell(*cell, counts));
        }
        if cell_visible(vis, &*cell, excluded, pattern) {
            proof {
                assert(exists|position: int| 0 <= position < rows@.len()
                    && (#[trigger] rows@[position]).1 == *cell
                    && cursor.remaining() == rows@.skip(position + 1)
                    && forall|earlier: int| 0 <= earlier < position ==>
                        !visible((#[trigger] rows@[earlier]).1, excluded@, pattern@));
            }
            return Some(cell);
        }
        processed = Ghost(processed@ + 1);
    }
}

/// Starts the stream over one size's rank order (nothing is added when no
/// entry is visible).
pub fn open_stream(streams: &mut Vec<Vec<Stream>>, vis: &mut Vis, rows: &Counts,
    excluded: &[u8], pattern: &[u8], Ghost(counts): Ghost<Counts>, Ghost(sizes): Ghost<Seq<i64>>,
)
    requires old(vis).ok(excluded@, pattern@),
        streams_state(old(streams)@, counts, excluded@, pattern@, sizes, Seq::empty()),
        rows.wf(), cursor_source(rows.entries(), counts), cursor_selected(rows.entries(), sizes),
    ensures final(vis).ok(excluded@, pattern@),
        streams_state(final(streams)@, counts, excluded@, pattern@, sizes, Seq::empty()),
        old(streams)@.len() <= final(streams)@.len() <= old(streams)@.len() + 1,
        forall|j: int| 0 <= j < old(streams)@.len() ==> final(streams)@[j] == old(streams)@[j],
        forall|pos: int| 0 <= pos < rows.entries().len() && visible((#[trigger] rows.entries()[pos]).1, excluded@, pattern@) ==>
            exists|j: int| 0 <= j < final(streams)@.len() && (#[trigger] final(streams)@[j])@.len() == 1
                && final(streams)@[j]@[0].orig@ == rows.entries(),
{
    let mut cursor = rows.cursor();
    let orig = Ghost(rows.entries());
    let head = match next_visible(&mut cursor, vis, excluded, pattern, Ghost(counts), Ghost(sizes)) { Some(value) => value, None => return };
    let ghost before = streams@;
    let mut slot = Vec::new();
    slot.push(Stream { cursor, head, finished: false, orig });
    proof {
        let s = slot@[0];
        let k = orig@.len() - s.cursor.remaining().len();
        assert(s.cursor.remaining() == orig@.skip(k));
        assert(orig@[k - 1].1 == *s.head);
        assert forall|pos: int| 0 <= pos < k - 1 && visible((#[trigger] s.orig@[pos]).1, excluded@, pattern@)
            implies cell_has(Seq::<Arc<Cell>>::empty(), s.orig@[pos].1) by {
            assert(!visible(orig@[pos].1, excluded@, pattern@));
        }
        assert(stream_state(s, counts, excluded@, pattern@, sizes, Seq::empty()));
    }
    streams.push(slot);
    proof {
        assert forall|j: int| 0 <= j < streams@.len() implies {
            let slot = #[trigger] streams@[j];
            slot@.len() == 1 && stream_state(slot@[0], counts, excluded@, pattern@, sizes, Seq::empty())
        } by {
            if j < before.len() { assert(streams@[j] == before[j]); }
        }
        assert forall|pos: int| 0 <= pos < orig@.len() && visible((#[trigger] orig@[pos]).1, excluded@, pattern@) implies
            exists|j: int| 0 <= j < streams@.len() && (#[trigger] streams@[j])@.len() == 1 && streams@[j]@[0].orig@ == orig@ by {
            let j = before.len() as int;
            assert(streams@[j]@[0].orig@ == orig@);
        }
    }
}

pub fn head_lt(vocab: &Vocab, left: &Cell, right: &Cell) -> (less: bool)
    requires vocab.ok(), left.group.valid(), right.group.valid(), left.consistent(), right.consistent(),
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
    if left.brand_id < 25 && right.brand_id < 25 && left.brand_id != right.brand_id {
        proof {
            assert(left.brand_id == left.group.brand_id && right.brand_id == right.group.brand_id);
            lemma_brand_code_sound((*left.group.brand)@);
            lemma_brand_code_sound((*right.group.brand)@);
            if left.brand_id < right.brand_id { lemma_brand_order(left.brand_id as int, right.brand_id as int); }
            else {
                lemma_brand_order(right.brand_id as int, left.brand_id as int);
                lemma_bytes_lt_asym((*left.group.brand)@, (*right.group.brand)@);
            }
        }
        return left.brand_id < right.brand_id;
    }
    let left_brand = brand_bytes(vocab, left);
    let right_brand = brand_bytes(vocab, right);
    if !bytes_eq_exec(left_brand, right_brand) {
        return bytes_lt_exec(left_brand, right_brand);
    }
    let left_kind = kind_bytes(vocab, left);
    let right_kind = kind_bytes(vocab, right);
    if !bytes_eq_exec(left_kind, right_kind) {
        return bytes_lt_exec(left_kind, right_kind);
    }
    proof { assert(left.size == left.group.size && right.size == right.group.size); }
    left.size < right.size
}

pub open spec fn stream_work(slot: Vec<Stream>) -> int {
    if slot@.len() == 1 && !slot@[0].finished { slot@[0].cursor.remaining().len() as int + 1 }
    else { 0 }
}

pub open spec fn winners_valid(winners: Seq<Option<usize>>, length: int) -> bool {
    forall|position: int| 0 <= position < winners.len() ==>
        ((#[trigger] winners[position]) matches Some(index) ==> index < length)
}

pub fn choose_stream(vocab: &Vocab, streams: &Vec<Vec<Stream>>, left: Option<usize>, right: Option<usize>, Ghost(counts): Ghost<Counts>) -> (winner: Option<usize>)
    requires vocab.ok(), forall|j: int| 0 <= j < streams@.len() ==> (#[trigger] streams@[j])@.len() == 1 && source_cell(*streams@[j]@[0].head, counts),
        left matches Some(index) ==> index < streams.len(),
        right matches Some(index) ==> index < streams.len(),
    ensures winner matches Some(index) ==> index < streams.len(),
{
    match (left, right) {
        (None, other) => other,
        (other, None) => other,
        (Some(first), Some(second)) => {
            if head_lt(vocab, &*streams[second][0].head, &*streams[first][0].head) { Some(second) }
            else { Some(first) }
        },
    }
}

pub open spec fn heads_source(streams: Seq<Vec<Stream>>, counts: Counts) -> bool {
    forall|j: int| 0 <= j < streams.len() ==> (#[trigger] streams[j])@.len() == 1 && source_cell(*streams[j]@[0].head, counts)
}

pub proof fn lemma_state_heads(streams: Seq<Vec<Stream>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>, out: Seq<Arc<Cell>>)
    requires streams_state(streams, counts, excluded, pattern, sizes, out),
    ensures heads_source(streams, counts),
{
}

pub fn merge_small(mut streams: Vec<Vec<Stream>>, Ghost(start): Ghost<Seq<Vec<Stream>>>, vis: &mut Vis, vocab: &Vocab, excluded: &[u8], pattern: &[u8], Ghost(counts): Ghost<Counts>, Ghost(sizes): Ghost<Seq<i64>>) -> (result: Option<Vec<Arc<Cell>>>)
    requires streams.len() <= 8, start == streams@, streams_state(streams@, counts, excluded@, pattern@, sizes, Seq::empty()),
        old(vis).ok(excluded@, pattern@), vocab.ok(),
    ensures final(vis).ok(excluded@, pattern@),
        result matches Some(cells) ==> cells_source(cells@, counts)
            && cells_selected(cells@, excluded@, pattern@, sizes)
            && forall|j: int, pos: int| 0 <= j < start.len() && 0 <= pos < start[j]@[0].orig@.len()
                && visible((#[trigger] start[j]@[0].orig@[pos]).1, excluded@, pattern@) ==>
                    cell_has(cells@, start[j]@[0].orig@[pos].1),
{
    let mut out: Vec<Arc<Cell>> = Vec::new();
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
            streams_state(streams@, counts, excluded@, pattern@, sizes, out@),
        decreases streams.len() - index,
    {
        if !streams[index][0].finished { winners.set(width + index, Some(index)); }
        index += 1;
    }
    proof { lemma_state_heads(streams@, counts, excluded@, pattern@, sizes, out@); }
    let mut node = width;
    while node > 1
        invariant 1 <= node <= width, 1 <= width <= 8, winners.len() == width * 2,
            winners_valid(winners@, streams.len() as int), heads_source(streams@, counts), vocab.ok(),
        decreases node,
    {
        node -= 1;
        let winner = choose_stream(vocab, &streams, winners[node * 2], winners[node * 2 + 1], Ghost(counts));
        winners.set(node, winner);
    }
    proof { lemma_sum_nonneg(streams@, |slot: Vec<Stream>| stream_work(slot)); }
    loop
        invariant sum(streams@, |slot: Vec<Stream>| stream_work(slot)) >= 0, vis.ok(excluded@, pattern@), vocab.ok(),
            streams_state(streams@, counts, excluded@, pattern@, sizes, out@),
            cells_source(out@, counts), cells_selected(out@, excluded@, pattern@, sizes),
            1 <= width <= 8, streams.len() <= width, winners.len() == width * 2,
            winners_valid(winners@, streams.len() as int),
            streams@.len() == start.len(),
            forall|j: int| 0 <= j < streams@.len() ==> (#[trigger] streams@[j])@[0].orig@ == start[j]@[0].orig@,
        decreases sum(streams@, |slot: Vec<Stream>| stream_work(slot)),
    {
        let before = Ghost(streams@);
        let before_out = Ghost(out@);
        let selected = match winners[1] {
            Some(value) => value,
            None => {
                let mut check = 0;
                while check < streams.len()
                    invariant check <= streams.len(), streams_state(streams@, counts, excluded@, pattern@, sizes, out@),
                        vis.ok(excluded@, pattern@), cells_source(out@, counts), cells_selected(out@, excluded@, pattern@, sizes),
                        streams@.len() == start.len(),
                        forall|j: int| 0 <= j < streams@.len() ==> (#[trigger] streams@[j])@[0].orig@ == start[j]@[0].orig@,
                        forall|j: int| 0 <= j < check ==> (#[trigger] streams@[j])@[0].finished,
                    decreases streams.len() - check,
                {
                    if !streams[check][0].finished { return None; }
                    check += 1;
                }
                proof {
                    assert forall|j: int, pos: int| 0 <= j < start.len() && 0 <= pos < start[j]@[0].orig@.len()
                        && visible((#[trigger] start[j]@[0].orig@[pos]).1, excluded@, pattern@) implies
                            cell_has(out@, start[j]@[0].orig@[pos].1) by {
                        let s = streams@[j]@[0];
                        assert(s.orig@ == start[j]@[0].orig@);
                        assert(s.finished);
                    }
                }
                return Some(out);
            },
        };
        if streams[selected][0].finished { return None; }
        let mut stream = match streams[selected].pop() { Some(value) => value, None => return None };
        let previous_stream = Ghost(stream);
        out.push(stream.head.clone());
        proof {
            let item = out@[before_out@.len() as int];
            assert(out@ =~= before_out@.push(item));
            lemma_push_extends(before_out@, out@, item);
            assert(*item == *stream.head);
            lemma_suffix_source(previous_stream@, counts, sizes);
        }
        let old_cursor = Ghost(stream.cursor.remaining());
        match next_visible(&mut stream.cursor, vis, excluded, pattern, Ghost(counts), Ghost(sizes)) {
            Some(head) => {
                stream.head = head;
                proof {
                    let position = choose|position: int| 0 <= position < old_cursor@.len()
                        && old_cursor@[position].1 == *stream.head
                        && stream.cursor.remaining() == old_cursor@.skip(position + 1)
                        && forall|earlier: int| 0 <= earlier < position ==>
                            !visible((#[trigger] old_cursor@[earlier]).1, excluded@, pattern@);
                    lemma_advance_some(previous_stream@, stream, position, counts, excluded@, pattern@, sizes, before_out@, out@);
                }
            },
            None => {
                stream.finished = true;
                proof {
                    lemma_advance_none(previous_stream@, stream, counts, excluded@, pattern@, sizes, before_out@, out@);
                }
            },
        }
        streams[selected].push(stream);
        proof {
            let s = streams@[selected as int]@[0];
            assert(s.orig@ == previous_stream@.orig@);
            assert(streams@ =~= before@.update(selected as int, streams@[selected as int]));
            assert(previous_stream@ == before@[selected as int]@[0]);
            assert(stream_state(s, counts, excluded@, pattern@, sizes, out@));
            assert forall|j: int| 0 <= j < streams@.len() implies {
                let slot = #[trigger] streams@[j];
                slot@.len() == 1 && stream_state(slot@[0], counts, excluded@, pattern@, sizes, out@)
            } by {
                if j != selected as int {
                    assert(streams@[j] == before@[j]);
                    lemma_state_extends(before@[j]@[0], counts, excluded@, pattern@, sizes, before_out@, out@);
                }
            }
            assert(cells_source(out@, counts)) by {
                assert forall|position: int| 0 <= position < out@.len() implies source_cell(*(#[trigger] out@[position]), counts) by {
                    if position < before_out@.len() { assert(out@[position] == before_out@[position]); }
                    else { assert(*out@[position] == *previous_stream@.head); }
                }
            }
            assert(cells_selected(out@, excluded@, pattern@, sizes)) by {
                assert forall|position: int| 0 <= position < out@.len() implies cell_selected(*(#[trigger] out@[position]), excluded@, pattern@, sizes) by {
                    if position < before_out@.len() { assert(out@[position] == before_out@[position]); }
                    else {
                        let head = *previous_stream@.head;
                        let kk = previous_stream@.orig@.len() - previous_stream@.cursor.remaining().len();
                        assert(previous_stream@.orig@[kk - 1].1 == head);
                        assert(*out@[position] == head);
                    }
                }
            }
            assert forall|j: int| 0 <= j < streams@.len() implies (#[trigger] streams@[j])@[0].orig@ == start[j]@[0].orig@ by {
                if j != selected as int { assert(streams@[j] == before@[j]); }
            }
            assert(stream_work(streams@[selected as int]) < stream_work(before@[selected as int]));
            lemma_small_stream_update(before@, streams@, selected as int);
            lemma_sum_nonneg(streams@, |slot: Vec<Stream>| stream_work(slot));
        }
        proof { lemma_state_heads(streams@, counts, excluded@, pattern@, sizes, out@); }
        let mut node = width + selected;
        winners.set(node, if streams[selected][0].finished { None } else { Some(selected) });
        while node > 1
            invariant 1 <= node < width * 2, 1 <= width <= 8, winners.len() == width * 2,
                winners_valid(winners@, streams.len() as int), heads_source(streams@, counts), vocab.ok(),
            decreases node,
        {
            node = node / 2;
            let winner = choose_stream(vocab, &streams, winners[node * 2], winners[node * 2 + 1], Ghost(counts));
            winners.set(node, winner);
        }
    }
}

pub open spec fn covered(streams: Seq<Vec<Stream>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>, n: int) -> bool {
    forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && (exists|q: int| 0 <= q < n && sizes[q] == counts.at(key).unwrap().group.size)
        && visible(counts.at(key).unwrap(), excluded, pattern) ==> streams_hold(streams, counts.at(key).unwrap())
}

pub open spec fn size_held(streams: Seq<Vec<Stream>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, size: i64) -> bool {
    forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size
        && visible(counts.at(key).unwrap(), excluded, pattern) ==> streams_hold(streams, counts.at(key).unwrap())
}

pub proof fn lemma_covered_step(old_streams: Seq<Vec<Stream>>, new_streams: Seq<Vec<Stream>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>,
    sizes: Seq<i64>, n: int)
    requires covered(old_streams, counts, excluded, pattern, sizes, n), 0 <= n < sizes.len(),
        size_held(new_streams, counts, excluded, pattern, sizes[n]),
        forall|j: int| 0 <= j < old_streams.len() ==> new_streams[j] == old_streams[j], old_streams.len() <= new_streams.len(),
    ensures covered(new_streams, counts, excluded, pattern, sizes, n + 1),
{
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && (exists|q: int| 0 <= q < n + 1 && sizes[q] == counts.at(key).unwrap().group.size)
        && visible(counts.at(key).unwrap(), excluded, pattern) implies streams_hold(new_streams, counts.at(key).unwrap()) by {
        let cell = counts.at(key).unwrap();
        let q = choose|q: int| 0 <= q < n + 1 && sizes[q] == cell.group.size;
        if q == n { assert(size_held(new_streams, counts, excluded, pattern, sizes[n])); }
        else {
            assert(streams_hold(old_streams, cell));
            let (j, pos) = choose|j: int, pos: int| 0 <= j < old_streams.len() && old_streams[j]@.len() == 1
                && 0 <= pos < old_streams[j]@[0].orig@.len() && old_streams[j]@[0].orig@[pos].1 == cell;
            assert(new_streams[j] == old_streams[j]);
        }
    }
}

pub proof fn lemma_held_same(streams: Seq<Vec<Stream>>, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>, n: int, size: i64)
    requires covered(streams, counts, excluded, pattern, sizes, n), 0 <= n < sizes.len(), size == sizes[n],
        exists|q: int| 0 <= q < n && sizes[q] == size,
    ensures size_held(streams, counts, excluded, pattern, size),
{
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size
        && visible(counts.at(key).unwrap(), excluded, pattern) implies streams_hold(streams, counts.at(key).unwrap()) by {
        let q = choose|q: int| 0 <= q < n && sizes[q] == size;
        assert(sizes[q] == counts.at(key).unwrap().group.size);
    }
}

pub proof fn lemma_held_open(streams: Seq<Vec<Stream>>, rows: &Counts, counts: Counts, excluded: Seq<u8>, pattern: Seq<u8>, size: i64)
    requires stream_valid(*rows, counts, size as int, None), rows.wf(),
        forall|pos: int| 0 <= pos < rows.entries().len() && visible((#[trigger] rows.entries()[pos]).1, excluded, pattern) ==>
            exists|j: int| 0 <= j < streams.len() && (#[trigger] streams[j])@.len() == 1 && streams[j]@[0].orig@ == rows.entries(),
    ensures size_held(streams, counts, excluded, pattern, size),
{
    rows.model();
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size
        && visible(counts.at(key).unwrap(), excluded, pattern) implies streams_hold(streams, counts.at(key).unwrap()) by {
        let cell = counts.at(key).unwrap();
        let rank = stream_key(*cell.group, cell.count as int, None);
        assert(stream_matches(*cell.group, size as int, None));
        assert(rows.at(rank) == Some(cell));
        let position = choose|position: int| 0 <= position < rows.entries().len()
            && (#[trigger] rows.entries()[position]).0 == rank;
        assert(rows.at(rank) == Some(rows.entries()[position].1));
        assert(rows.entries()[position].1 == cell);
        let j = choose|j: int| 0 <= j < streams.len() && (#[trigger] streams[j])@.len() == 1 && streams[j]@[0].orig@ == rows.entries();
        assert(streams[j]@[0].orig@[position].1 == cell);
    }
}

pub fn read(ranks: &Ranks, vis: &mut Vis, vocab: &Vocab, excluded: &[u8], pattern: &[u8], sizes: &[i64], Ghost(counts): Ghost<Counts>) -> (result: Option<Vec<Arc<Cell>>>)
    requires sizes.len() <= 8, ranks_valid(*ranks, counts), old(vis).ok(excluded@, pattern@), vocab.ok(),
    ensures final(vis).ok(excluded@, pattern@),
        result matches Some(cells) ==> cells_source(cells@, counts) && cells_selected(cells@, excluded@, pattern@, sizes@)
            && cells_cover(cells@, counts, excluded@, pattern@, sizes@),
{
    let mut streams: Vec<Vec<Stream>> = Vec::new();
    let mut index = 0;
    while index < sizes.len()
        invariant index <= sizes.len(), sizes.len() <= 8, ranks_valid(*ranks, counts), vis.ok(excluded@, pattern@),
            streams_state(streams@, counts, excluded@, pattern@, sizes@, Seq::empty()), streams.len() <= index,
            covered(streams@, counts, excluded@, pattern@, sizes@, index as int),
        decreases sizes.len() - index,
    {
        let size = sizes[index];
        let mut earlier = 0;
        let mut duplicate = false;
        while earlier < index
            invariant earlier <= index, index < sizes.len(),
                duplicate ==> exists|q: int| 0 <= q < index && sizes@[q] == size,
                !duplicate ==> forall|q: int| 0 <= q < earlier ==> sizes@[q] != size,
            decreases index - earlier,
        {
            if sizes[earlier] == size { duplicate = true; break; }
            earlier += 1;
        }
        let before = Ghost(streams@);
        if duplicate {
            proof {
                lemma_held_same(streams@, counts, excluded@, pattern@, sizes@, index as int, size);
                lemma_covered_step(before@, streams@, counts, excluded@, pattern@, sizes@, index as int);
            }
            index += 1;
            continue;
        }
        let size_rows = match ranks.get(&Key::number(size)) {
            Some(value) => value,
            None => {
                proof {
                    assert(size_held(streams@, counts, excluded@, pattern@, size)) by {
                        assert forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size
                            && visible(counts.at(key).unwrap(), excluded@, pattern@) implies streams_hold(streams@, counts.at(key).unwrap()) by {
                            assert(ranks.at((size as int, Seq::<u8>::empty())).is_some());
                        }
                    }
                    lemma_covered_step(before@, streams@, counts, excluded@, pattern@, sizes@, index as int);
                }
                index += 1;
                continue;
            },
        };
        proof {
            assert(size_valid(*size_rows, counts, size as int));
            lemma_rank_source(&size_rows.rows, counts, size as int, None);
            size_rows.rows.model();
            assert forall|position: int| 0 <= position < size_rows.rows.entries().len() implies
                sizes@.contains((#[trigger] size_rows.rows.entries()[position]).1.group.size) by {
                let entry = size_rows.rows.entries()[position];
                assert(size_rows.rows.at(entry.0) == Some(entry.1));
                assert(stream_matches(*entry.1.group, size as int, None));
                assert(sizes@[index as int] == size);
            }
            assert(stream_valid(size_rows.rows, counts, size as int, None));
        }
        open_stream(&mut streams, vis, &size_rows.rows, excluded, pattern, Ghost(counts), Ghost(sizes@));
        proof {
            lemma_held_open(streams@, &size_rows.rows, counts, excluded@, pattern@, size);
            lemma_covered_step(before@, streams@, counts, excluded@, pattern@, sizes@, index as int);
        }
        index += 1;
    }
    let result = merge_small(streams, Ghost(streams@), vis, vocab, excluded, pattern, Ghost(counts), Ghost(sizes@));
    proof {
        if result is Some {
            let cells = result->Some_0;
            assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
                && sizes@.contains(counts.at(key).unwrap().group.size)
                && visible(counts.at(key).unwrap(), excluded@, pattern@) implies cell_has(cells@, counts.at(key).unwrap()) by {
                let cell = counts.at(key).unwrap();
                let q = choose|q: int| 0 <= q < sizes@.len() && sizes@[q] == cell.group.size;
                assert(streams_hold(streams@, cell));
            }
        }
    }
    result
}

pub open spec fn streams_hold(streams: Seq<Vec<Stream>>, cell: Cell) -> bool {
    exists|j: int, pos: int| 0 <= j < streams.len() && streams[j]@.len() == 1
        && 0 <= pos < streams[j]@[0].orig@.len() && (#[trigger] streams[j]@[0].orig@[pos]).1 == cell
}

}
