use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::order::*;
use verus_prooflib::ordered::{Key, KeyView};
use crate::counts::*;
use crate::encoding::lemma_group_text_identity;
use crate::gen_obligations::{out_view, params, seq_view_i64};
use crate::gen_spec::*;
use crate::model::*;
use crate::semantic_proofs::output_model;

verus! {

pub type RuntimeRow = (Vec<u8>, Vec<u8>, i64, i64);

pub open spec fn row_view(row: RuntimeRow) -> OutRow {
    OutRow { p_brand: row.0@, p_type: row.1@, p_size: row.2 as int, supplier_cnt: row.3 as int }
}

pub open spec fn row_key(row: OutRow) -> KeyView {
    group_key(row.p_brand, row.p_type, row.p_size)
}

pub open spec fn rows_source(rows: Seq<OutRow>, counts: Counts) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> {
        let row = #[trigger] rows[position];
        row.supplier_cnt > 0 && row.supplier_cnt == count_at(counts, row_key(row))
    }
}

pub open spec fn cell_row(cell: Cell) -> OutRow {
    OutRow { p_brand: (*cell.group.brand)@, p_type: (*cell.group.kind)@,
        p_size: cell.group.size as int, supplier_cnt: cell.count as int }
}

pub open spec fn rows_selected(rows: Seq<OutRow>, excluded: Seq<u8>, sizes: Seq<i64>) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> {
        let row = #[trigger] rows[position];
        row.p_brand != excluded && seq_view_i64(sizes).contains(row.p_size)
    }
}

pub open spec fn rows_ordered(rows: Seq<OutRow>) -> bool {
    forall|first: int, last: int| 0 <= first < last < rows.len() ==>
        !same_group(#[trigger] rows[first], #[trigger] rows[last]) && !row_lt(rows[last], rows[first])
}

pub fn materialize(cells: &Vec<Arc<Cell>>, Ghost(counts): Ghost<Counts>, Ghost(excluded): Ghost<Seq<u8>>, Ghost(sizes): Ghost<Seq<i64>>) -> (result: Option<Vec<RuntimeRow>>)
    requires crate::output::cells_source(cells@, counts), crate::output::cells_selected(cells@, excluded, sizes),
    ensures result matches Some(rows) ==> rows_source(out_view(rows@), counts)
        && rows_selected(out_view(rows@), excluded, sizes) && rows_ordered(out_view(rows@)),
{
    proof { lemma_ext_order(); }
    let mut rows = Vec::new();
    let mut position = 0;
    while position < cells.len()
        invariant position <= cells.len(), rows.len() == position,
            crate::output::cells_source(cells@, counts), rows_source(out_view(rows@), counts),
            crate::output::cells_selected(cells@, excluded, sizes), rows_selected(out_view(rows@), excluded, sizes),
            strict_order(ext_order()),
            forall|index: int| 0 <= index < position ==>
                (#[trigger] out_view(rows@)[index]) == cell_row(*cells@[index]),
            forall|first: int, last: int| 0 <= first < last < position ==>
                !same_group(#[trigger] out_view(rows@)[first], #[trigger] out_view(rows@)[last])
                    && row_lt(out_view(rows@)[first], out_view(rows@)[last]),
        decreases cells.len() - position,
    {
        let cell = &*cells[position];
        let before = Ghost(rows@);
        if position > 0 && !crate::output::head_lt(&*cells[position - 1], cell) { return None; }
        proof { assert(crate::output::source_cell(*cell, counts)); }
        rows.push((copy_bytes(cell.group.brand.as_slice()), copy_bytes(cell.group.kind.as_slice()), cell.group.size, cell.count));
        proof {
            assert forall|index: int| 0 <= index < rows.len() implies {
                let row = #[trigger] out_view(rows@)[index];
                row.supplier_cnt > 0 && row.supplier_cnt == count_at(counts, row_key(row))
            } by {
                if index < position { assert(out_view(rows@)[index] == out_view(before@)[index]); }
                else { assert(row_key(out_view(rows@)[index]) == cell.group.key()); }
            }
            assert(out_view(rows@)[position as int] == cell_row(*cell));
            assert forall|index: int| 0 <= index < rows.len() implies {
                let row = #[trigger] out_view(rows@)[index];
                row.p_brand != excluded && seq_view_i64(sizes).contains(row.p_size)
            } by {
                if index < position { assert(out_view(rows@)[index] == out_view(before@)[index]); }
                else {
                    assert(crate::output::cell_selected(*cell, excluded, sizes));
                    let selected = choose|selected: int| 0 <= selected < sizes.len()
                        && (#[trigger] sizes[selected]) == cell.group.size;
                    assert(seq_view_i64(sizes)[selected] == cell.group.size as int);
                }
            }
            assert forall|index: int| 0 <= index < rows.len() implies
                (#[trigger] out_view(rows@)[index]) == cell_row(*cells@[index]) by {
                if index < position { assert(out_view(rows@)[index] == out_view(before@)[index]); }
            }
            let current = out_view(rows@)[position as int];
            assert forall|earlier: int| 0 <= earlier < position implies
                !same_group(#[trigger] out_view(rows@)[earlier], current)
                    && row_lt(out_view(rows@)[earlier], current) by {
                if earlier < position as int - 1 {
                    let middle = out_view(rows@)[position as int - 1];
                    assert(out_view(rows@)[earlier] == out_view(before@)[earlier]);
                    assert(middle == out_view(before@)[position as int - 1]);
                    lemma_row_lt_ext(out_view(rows@)[earlier], middle);
                    lemma_row_lt_ext(middle, current);
                    lemma_row_lt_ext(out_view(rows@)[earlier], current);
                }
                if same_group(out_view(rows@)[earlier], current) {
                    assert(out_view(rows@)[earlier].supplier_cnt == current.supplier_cnt);
                    lemma_bytes_lt_irrefl(current.p_brand);
                    lemma_bytes_lt_irrefl(current.p_type);
                }
            }
            assert forall|first: int, last: int| 0 <= first < last < rows.len() implies
                !same_group(#[trigger] out_view(rows@)[first], #[trigger] out_view(rows@)[last])
                    && row_lt(out_view(rows@)[first], out_view(rows@)[last]) by {
                if last < position {
                    assert(out_view(rows@)[first] == out_view(before@)[first]);
                    assert(out_view(rows@)[last] == out_view(before@)[last]);
                }
            }
        }
        position += 1;
    }
    proof {
        assert forall|first: int, last: int| 0 <= first < last < rows.len() implies
            !same_group(#[trigger] out_view(rows@)[first], #[trigger] out_view(rows@)[last])
                && !row_lt(out_view(rows@)[last], out_view(rows@)[first]) by {
            lemma_row_lt_ext(out_view(rows@)[first], out_view(rows@)[last]);
            lemma_row_lt_ext(out_view(rows@)[last], out_view(rows@)[first]);
        }
    }
    Some(rows)
}

pub open spec fn contains_key(seen: Seq<OutRow>, key: KeyView) -> bool {
    exists|position: int| 0 <= position < seen.len() && row_key(#[trigger] seen[position]) == key
}

pub open spec fn stream_covered(rows: Counts, seen: Seq<OutRow>, excluded: Seq<u8>) -> bool {
    forall|key: KeyView| #[trigger] rows.at(key).is_some()
        && (*rows.at(key).unwrap().group.brand)@ != excluded ==>
            contains_key(seen, rows.at(key).unwrap().group.key())
}

pub open spec fn size_covered(counts: Counts, seen: Seq<OutRow>, size: int, excluded: Seq<u8>) -> bool {
    forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && counts.at(key).unwrap().group.size == size && (*counts.at(key).unwrap().group.brand)@ != excluded ==>
            contains_key(seen, key)
}

pub fn cell_before_row(cell: &Cell, row: &RuntimeRow) -> bool
{
    if cell.count != row.3 { return cell.count > row.3; }
    if !bytes_eq_exec(cell.group.brand.as_slice(), row.0.as_slice()) {
        return bytes_lt_exec(cell.group.brand.as_slice(), row.0.as_slice());
    }
    if !bytes_eq_exec(cell.group.kind.as_slice(), row.1.as_slice()) {
        return bytes_lt_exec(cell.group.kind.as_slice(), row.1.as_slice());
    }
    cell.group.size < row.2
}

pub fn cell_equals_row(cell: &Cell, row: &RuntimeRow) -> (equal: bool)
    ensures equal ==> row_key(row_view(*row)) == cell.group.key(),
{
    cell.count == row.3 && cell.group.size == row.2
        && bytes_eq_exec(cell.group.brand.as_slice(), row.0.as_slice())
        && bytes_eq_exec(cell.group.kind.as_slice(), row.1.as_slice())
}

pub fn find_cell(seen: &Vec<RuntimeRow>, cell: &Cell, next: &mut usize) -> (found: bool)
    requires *old(next) <= seen.len(),
    ensures *final(next) <= seen.len(),
        found ==> contains_key(out_view(seen@), cell.group.key()),
{
    let mut lower = *next;
    let mut upper = seen.len();
    let mut distance = 1;
    let mut bracketed = false;
    while lower < upper && !bracketed
        invariant lower <= upper <= seen.len(), *next <= seen.len(), distance > 0,
            !bracketed ==> upper == seen.len(),
        decreases if bracketed { 0int } else { seen.len() - lower + 1 },
    {
        let row = &seen[lower];
        if cell_equals_row(cell, row) {
            proof { assert(row_key(out_view(seen@)[lower as int]) == cell.group.key()); }
            *next = lower + 1;
            return true;
        }
        if cell_before_row(cell, row) {
            upper = lower;
            bracketed = true;
        } else if distance >= seen.len() - lower {
            lower += 1;
            bracketed = true;
        } else {
            let probe = lower + distance;
            if cell_before_row(cell, &seen[probe]) || cell_equals_row(cell, &seen[probe]) {
                upper = probe + 1;
                lower += 1;
                bracketed = true;
            } else {
                lower = probe + 1;
                if distance <= (seen.len() - lower) / 2 { distance = distance * 2; }
            }
        }
    }
    while lower < upper
        invariant lower <= upper <= seen.len(), *next <= seen.len(),
        decreases upper - lower,
    {
        let middle = lower + (upper - lower) / 2;
        let row = &seen[middle];
        if cell_equals_row(cell, row) {
            proof {
                assert(row_key(out_view(seen@)[middle as int]) == cell.group.key());
            }
            *next = middle + 1;
            return true;
        } else if cell_before_row(cell, row) { upper = middle; }
        else { lower = middle + 1; }
    }
    false
}

pub fn check_stream(rows: &Counts, seen: &Vec<RuntimeRow>, excluded: &[u8]) -> (ok: bool)
    requires rows.wf(),
        forall|key: KeyView| #[trigger] rows.at(key).is_some() ==> rows.at(key).unwrap().group.valid(),
    ensures ok ==> stream_covered(*rows, out_view(seen@), excluded@),
{
    let mut cursor = rows.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    let mut next = 0;
    loop
        invariant rows.wf(), next <= seen.len(), 0 <= processed@ <= rows.entries().len(),
            cursor.remaining() == rows.entries().skip(processed@),
            forall|key: KeyView| #[trigger] rows.at(key).is_some() ==> rows.at(key).unwrap().group.valid(),
            forall|position: int| 0 <= position < processed@ ==>
                (*((#[trigger] rows.entries()[position]).1.group.brand))@ != excluded@ ==>
                    contains_key(out_view(seen@), rows.entries()[position].1.group.key()),
        decreases cursor.remaining().len(),
    {
        let (key, cell) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == rows.entries().len());
                    rows.model();
                    assert forall|key: KeyView| #[trigger] rows.at(key).is_some()
                        && (*rows.at(key).unwrap().group.brand)@ != excluded@ implies
                            contains_key(out_view(seen@), rows.at(key).unwrap().group.key()) by {
                        let position = choose|position: int| 0 <= position < rows.entries().len()
                            && (#[trigger] rows.entries()[position]).0 == key;
                        assert(rows.at(key) == Some(rows.entries()[position].1));
                    }
                }
                return true;
            },
        };
        proof {
            rows.model();
            assert((key.view(), *cell) == rows.entries()[processed@]);
            assert(rows.at(key.view()) == Some(*cell));
        }
        if !bytes_eq_exec(cell.group.brand.as_slice(), excluded)
            && !find_cell(seen, &*cell, &mut next) { return false; }
        processed = Ghost(processed@ + 1);
    }
}

pub fn check_size(profile: &SizeRanks, counts: &Counts, seen: &Vec<RuntimeRow>, size: i64, excluded: &[u8]) -> (ok: bool)
    requires positive_counts(*counts), size_valid(*profile, *counts, size as int),
    ensures ok ==> size_covered(*counts, out_view(seen@), size as int, excluded@),
{
    let excluded_key = Key::text(copy_bytes(excluded));
    let excluded_groups = match profile.brands.get(&excluded_key) { Some(brand) => brand.groups, None => 0 };
    if excluded_groups >= 0 && excluded_groups <= profile.groups
        && excluded_groups <= profile.groups - excluded_groups {
        proof {
            assert forall|key: KeyView| #[trigger] profile.rows.at(key).is_some() implies
                profile.rows.at(key).unwrap().group.valid() by {
                let cell = profile.rows.at(key).unwrap();
                assert(counts.at(cell.group.key()) == Some(cell));
            }
        }
        if !check_stream(&profile.rows, seen, excluded) { return false; }
        proof {
            assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
                && counts.at(key).unwrap().group.size == size && (*counts.at(key).unwrap().group.brand)@ != excluded@ implies
                    contains_key(out_view(seen@), key) by {
                let cell = counts.at(key).unwrap();
                let rank = stream_key(*cell.group, cell.count as int, None);
                assert(profile.rows.at(rank) == Some(cell));
            }
        }
        return true;
    }
    let mut cursor = profile.brands.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant positive_counts(*counts), size_valid(*profile, *counts, size as int),
            0 <= processed@ <= profile.brands.entries().len(),
            cursor.remaining() == profile.brands.entries().skip(processed@),
            forall|position: int| 0 <= position < processed@ ==>
                stream_covered((#[trigger] profile.brands.entries()[position]).1.rows, out_view(seen@), excluded@),
        decreases cursor.remaining().len(),
    {
        let (brand, stream) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == profile.brands.entries().len());
                    profile.brands.model();
                    assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
                        && counts.at(key).unwrap().group.size == size && (*counts.at(key).unwrap().group.brand)@ != excluded@ implies
                            contains_key(out_view(seen@), key) by {
                        let cell = counts.at(key).unwrap();
                        let brand = (0int, (*cell.group.brand)@);
                        assert(profile.brands.at(brand).is_some());
                        let position = choose|position: int| 0 <= position < profile.brands.entries().len()
                            && (#[trigger] profile.brands.entries()[position]).0 == brand;
                        let rows = profile.brands.entries()[position].1.rows;
                        assert(stream_valid(rows, *counts, size as int, Some(brand.1)));
                        let rank = stream_key(*cell.group, cell.count as int, Some(brand.1));
                        assert(rows.at(rank) == Some(cell));
                        assert(contains_key(out_view(seen@), cell.group.key()));
                    }
                }
                return true;
            },
        };
        proof {
            profile.brands.model();
            assert((brand.view(), *stream) == profile.brands.entries()[processed@]);
            assert(profile.brands.at(brand.view()) == Some(*stream));
            assert(stream_valid(stream.rows, *counts, size as int, Some(brand.view().1)));
            assert forall|key: KeyView| #[trigger] stream.rows.at(key).is_some() implies
                stream.rows.at(key).unwrap().group.valid() by {
                let cell = stream.rows.at(key).unwrap();
                assert(counts.at(cell.group.key()) == Some(cell));
            }
        }
        if bytes_eq_exec(brand.text.as_slice(), excluded) {
            proof {
                assert(stream_covered(stream.rows, out_view(seen@), excluded@)) by {
                    assert forall|key: KeyView| #[trigger] stream.rows.at(key).is_some()
                        && (*stream.rows.at(key).unwrap().group.brand)@ != excluded@ implies
                            contains_key(out_view(seen@), stream.rows.at(key).unwrap().group.key()) by {};
                }
            }
        } else if !check_stream(&stream.rows, seen, excluded) { return false; }
        processed = Ghost(processed@ + 1);
    }
}

pub fn validate(counts: &Counts, ranks: &Ranks, rows: &Vec<RuntimeRow>, excluded: &[u8], sizes: &[i64]) -> (ok: bool)
    requires ranks_valid(*ranks, *counts), sizes.len() == 8, rows_source(out_view(rows@), *counts),
        rows_selected(out_view(rows@), excluded@, sizes@), rows_ordered(out_view(rows@)),
    ensures ok ==> output_model(*counts,
        params(excluded@, sizes@[0] as int, sizes@[1] as int, sizes@[2] as int, sizes@[3] as int,
            sizes@[4] as int, sizes@[5] as int, sizes@[6] as int, sizes@[7] as int), out_view(rows@)),
{
    hide(rows_source);
    let seen = rows;
    let mut position = 0;
    while position < sizes.len()
        invariant position <= sizes.len(), ranks_valid(*ranks, *counts),
            forall|earlier: int| 0 <= earlier < position ==>
                size_covered(*counts, out_view(seen@), (#[trigger] sizes@[earlier]) as int, excluded@),
        decreases sizes.len() - position,
    {
        let size = sizes[position];
        let mut earlier = 0;
        let mut duplicate = false;
        while earlier < position
            invariant earlier <= position, position < sizes.len(),
                duplicate ==> size_covered(*counts, out_view(seen@), size as int, excluded@),
                forall|checked: int| 0 <= checked < position ==>
                    size_covered(*counts, out_view(seen@), (#[trigger] sizes@[checked]) as int, excluded@),
            decreases position - earlier,
        {
            if sizes[earlier] == size { duplicate = true; }
            earlier += 1;
        }
        if duplicate { position += 1; continue; }
        match ranks.get(&Key::number(size)) {
            Some(profile) => if !check_size(&*profile, counts, seen, size, excluded) { return false; },
            None => {
                proof {
                    assert(size_covered(*counts, out_view(seen@), size as int, excluded@)) by {
                        assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
                            && counts.at(key).unwrap().group.size == size && (*counts.at(key).unwrap().group.brand)@ != excluded@ implies
                                contains_key(out_view(seen@), key) by {
                            assert(ranks.at((size as int, Seq::<u8>::empty())).is_some());
                        }
                    }
                }
            },
        }
        position += 1;
    }
    proof {
        reveal(rows_source);
        assert forall|index: int| 0 <= index < rows.len() implies {
            let row = #[trigger] out_view(rows@)[index];
            row.p_brand != excluded@ && row.supplier_cnt > 0
                && row.supplier_cnt == count_at(*counts, row_key(row))
                && (row.p_size == sizes@[0] as int || row.p_size == sizes@[1] as int
                    || row.p_size == sizes@[2] as int || row.p_size == sizes@[3] as int
                    || row.p_size == sizes@[4] as int || row.p_size == sizes@[5] as int
                    || row.p_size == sizes@[6] as int || row.p_size == sizes@[7] as int)
        } by {
            let row = out_view(rows@)[index];
            assert(seq_view_i64(sizes@).contains(row.p_size));
            let selected = choose|selected: int| 0 <= selected < sizes.len()
                && (#[trigger] seq_view_i64(sizes@)[selected]) == row.p_size;
            assert(sizes@[selected] as int == row.p_size);
        }
        let parameters = params(excluded@, sizes@[0] as int, sizes@[1] as int, sizes@[2] as int, sizes@[3] as int,
            sizes@[4] as int, sizes@[5] as int, sizes@[6] as int, sizes@[7] as int);
        assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            #[trigger] count_at(*counts, group_key(brand, kind, size)) > 0 && brand != parameters.p1
            && (size == parameters.p2 || size == parameters.p3 || size == parameters.p4 || size == parameters.p5
                || size == parameters.p6 || size == parameters.p7 || size == parameters.p8 || size == parameters.p9) implies
                exists|index: int| 0 <= index < out_view(rows@).len() && (#[trigger] out_view(rows@)[index]).p_brand == brand
                    && out_view(rows@)[index].p_type == kind && out_view(rows@)[index].p_size == size by {
            let key = group_key(brand, kind, size);
            let cell = counts.at(key).unwrap();
            lemma_group_text_identity(brand, kind, (*cell.group.brand)@, (*cell.group.kind)@);
            assert(size_covered(*counts, out_view(seen@), size, excluded@));
            assert(contains_key(out_view(seen@), key));
            let index = choose|index: int| 0 <= index < out_view(rows@).len()
                && row_key(#[trigger] out_view(rows@)[index]) == key;
            assert(row_key(out_view(rows@)[index]) == key);
            lemma_group_text_identity(brand, kind, out_view(rows@)[index].p_brand, out_view(rows@)[index].p_type);
        }
    }
    true
}

}
