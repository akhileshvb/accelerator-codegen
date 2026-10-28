use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::order::*;
use verus_prooflib::ordered::{Key, KeyView};
use crate::counts::*;
use crate::vocab::*;
use crate::output::*;
use crate::encoding::lemma_group_text_identity;
use crate::gen_obligations::{out_view, params, seq_view_i64};
use crate::gen_spec::*;
use crate::model::*;
use crate::semantic_proofs::output_model;
use verus_common::sqlfns::like_match;
use verus_prooflib::like_match_exec;

verus! {

pub type RuntimeRow = (Vec<u8>, Vec<u8>, i64, i128);

pub open spec fn row_view(row: RuntimeRow) -> OutRow {
    OutRow { p_brand: row.0@, p_type: row.1@, p_size: row.2 as int, supplier_cnt: row.3 as int }
}

pub open spec fn rows_view(rows: Seq<RuntimeRow>) -> Seq<OutRow> {
    rows.map_values(|row: RuntimeRow| row_view(row))
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

pub open spec fn rows_selected(rows: Seq<OutRow>, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> {
        let row = #[trigger] rows[position];
        row.p_brand != excluded && !like_match(row.p_type, pattern) && seq_view_i64(sizes).contains(row.p_size)
    }
}

pub open spec fn rows_ordered(rows: Seq<OutRow>) -> bool {
    forall|first: int, last: int| 0 <= first < last < rows.len() ==>
        !same_group(#[trigger] rows[first], #[trigger] rows[last]) && !row_lt(rows[last], rows[first])
}

pub open spec fn arc_row(cell: Arc<Cell>) -> OutRow {
    cell_row(*cell)
}

pub open spec fn sourced(cells: Seq<Arc<Cell>>, cell: Arc<Cell>, bound: int) -> bool {
    exists|k: int| 0 <= k < bound && *(#[trigger] cells[k]) == *cell
}

pub open spec fn row_at(cells: Seq<Arc<Cell>>, position: int) -> OutRow {
    cell_row(*cells[position])
}

pub open spec fn cells_sorted(cells: Seq<Arc<Cell>>) -> bool {
    forall|first: int, last: int| 0 <= first < last < cells.len() ==>
        ext_order()(cell_row(*#[trigger] cells[first]), cell_row(*#[trigger] cells[last]))
}

proof fn lemma_unordered_same(left: Arc<Cell>, right: Arc<Cell>, counts: Counts)
    requires source_cell(*left, counts), source_cell(*right, counts),
        !ext_order()(cell_row(*left), cell_row(*right)), !ext_order()(cell_row(*right), cell_row(*left)),
    ensures *left == *right,
{
    let a = arc_row(left);
    let b = arc_row(right);
    if a.p_brand != b.p_brand { lemma_bytes_lt_total(a.p_brand, b.p_brand); }
    if a.p_type != b.p_type { lemma_bytes_lt_total(a.p_type, b.p_type); }
    assert(left.group.key() == right.group.key());
    assert(counts.at(left.group.key()) == Some(*left));
    assert(counts.at(right.group.key()) == Some(*right));
}

/// Insertion by rank order: a total fallback for a merge whose output is not strictly ordered.
pub fn sort_cells(vocab: &Vocab, cells: &Vec<Arc<Cell>>, Ghost(counts): Ghost<Counts>) -> (out: Vec<Arc<Cell>>)
    requires vocab.ok(), cells_source(cells@, counts),
    ensures cells_source(out@, counts), cells_sorted(out@),
        forall|position: int| 0 <= position < out@.len() ==> cell_has(cells@, *#[trigger] out@[position]),
        forall|cell: Cell| cell_has(cells@, cell) ==> cell_has(out@, cell),
{
    proof { lemma_ext_order(); }
    let mut out: Vec<Arc<Cell>> = Vec::new();
    let mut index = 0;
    while index < cells.len()
        invariant index <= cells.len(), vocab.ok(), cells_source(cells@, counts), cells_source(out@, counts),
            cells_sorted(out@), strict_order(ext_order()),
            forall|position: int| 0 <= position < out@.len() ==> sourced(cells@, #[trigger] out@[position], index as int),
            forall|k: int| 0 <= k < index ==> cell_has(out@, *(#[trigger] cells@[k])),
        decreases cells.len() - index,
    {
        let item = &cells[index];
        let before = Ghost(out@);
        let mut pos = 0;
        let mut found = false;
        let mut equal = false;
        while pos < out.len() && !found && !equal
            invariant pos <= out.len(), out@ == before@, vocab.ok(),
                cells_source(out@, counts), source_cell(*cells@[index as int], counts),
                *item == cells@[index as int],
                found ==> pos < out.len() && ext_order()(cell_row(*cells@[index as int]), cell_row(*out@[pos as int])),
                equal ==> pos < out.len() && !ext_order()(cell_row(*cells@[index as int]), cell_row(*out@[pos as int]))
                    && !ext_order()(cell_row(*out@[pos as int]), cell_row(*cells@[index as int])),
                forall|k: int| 0 <= k < pos ==> ext_order()(cell_row(*(#[trigger] out@[k])), cell_row(*cells@[index as int])),
            decreases out.len() - pos + if found || equal { 0int } else { 1int },
        {
            if head_lt(vocab, &*item, &*out[pos]) { found = true; }
            else if !head_lt(vocab, &*out[pos], &*item) { equal = true; }
            else { pos += 1; }
        }
        if equal {
            proof {
                lemma_unordered_same(cells@[index as int], out@[pos as int], counts);
                assert(cell_has(out@, *cells@[index as int]));
            }
        } else {
            proof {
                if found { assert(ext_order()(cell_row(*cells@[index as int]), cell_row(*out@[pos as int]))); }
                before@.insert_ensures(pos as int, *item);
            }
            out.insert(pos, item.clone());
            proof {
                let x = cells@[index as int];
                assert forall|first: int, last: int| 0 <= first < last < out@.len() implies
                    ext_order()(cell_row(*#[trigger] out@[first]), cell_row(*#[trigger] out@[last])) by {
                    if last < pos as int { assert(out@[first] == before@[first]); assert(out@[last] == before@[last]); }
                    else if last == pos as int { assert(out@[first] == before@[first]); }
                    else if first < pos as int {
                        assert(out@[first] == before@[first]);
                        assert(out@[last] == before@[last - 1]);
                    } else if first == pos as int {
                        assert(out@[last] == before@[last - 1]);
                        assert(found);
                        assert(ext_order()(cell_row(*x), cell_row(*before@[pos as int])));
                        if last - 1 > pos as int {
                            assert(ext_order()(cell_row(*before@[pos as int]), cell_row(*before@[last - 1])));
                        }
                    } else {
                        assert(out@[first] == before@[first - 1]);
                        assert(out@[last] == before@[last - 1]);
                    }
                }
                assert forall|position: int| 0 <= position < out@.len() implies source_cell(*#[trigger] out@[position], counts) by {
                    if position < pos as int { assert(out@[position] == before@[position]); }
                    else if position > pos as int { assert(out@[position] == before@[position - 1]); }
                }
                assert forall|position: int| 0 <= position < out@.len() implies
                    sourced(cells@, #[trigger] out@[position], index as int + 1) by {
                    if position < pos as int {
                        assert(out@[position] == before@[position]);
                        let k = choose|k: int| 0 <= k < index && *cells@[k] == *before@[position];
                    }
                    else if position == pos as int { assert(*cells@[index as int] == *out@[position]); }
                    else {
                        assert(out@[position] == before@[position - 1]);
                        let k = choose|k: int| 0 <= k < index && *cells@[k] == *before@[position - 1];
                    }
                }
                assert forall|k: int| 0 <= k < index + 1 implies cell_has(out@, *(#[trigger] cells@[k])) by {
                    if k < index as int {
                        let p = choose|p: int| 0 <= p < before@.len() && *(#[trigger] before@[p]) == *cells@[k];
                        if p < pos as int { assert(out@[p] == before@[p]); } else { assert(out@[p + 1] == before@[p]); }
                    } else { assert(out@[pos as int] == item); }
                }
            }
        }
        proof {
            if equal {
                assert forall|k: int| 0 <= k < index + 1 implies cell_has(out@, *(#[trigger] cells@[k])) by {
                    if k < index as int {} else { assert(cell_has(out@, *cells@[index as int])); }
                }
                assert forall|position: int| 0 <= position < out@.len() implies
                    sourced(cells@, #[trigger] out@[position], index as int + 1) by {
                    let k = choose|k: int| 0 <= k < index && *cells@[k] == *out@[position];
                }
            }
        }
        index += 1;
    }
    proof {
        assert forall|position: int| 0 <= position < out@.len() implies cell_has(cells@, *#[trigger] out@[position]) by {
            let k = choose|k: int| 0 <= k < index && *cells@[k] == *out@[position];
            assert(*cells@[k] == *out@[position]);
        }
        assert forall|cell: Cell| cell_has(cells@, cell) implies cell_has(out@, cell) by {
            let k = choose|k: int| 0 <= k < cells.len() && *(#[trigger] cells@[k]) == cell;
        }
    }
    out
}

pub proof fn lemma_sorted_selected(cells: Seq<Arc<Cell>>, sorted: Seq<Arc<Cell>>, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>)
    requires cells_selected(cells, excluded, pattern, sizes),
        forall|position: int| 0 <= position < sorted.len() ==> cell_has(cells, *#[trigger] sorted[position]),
    ensures cells_selected(sorted, excluded, pattern, sizes),
{
    assert forall|position: int| 0 <= position < sorted.len() implies
        cell_selected(*#[trigger] sorted[position], excluded, pattern, sizes) by {
        let index = choose|index: int| 0 <= index < cells.len() && *(#[trigger] cells[index]) == *sorted[position];
    }
}

pub fn materialize(vocab: &Vocab, cells: &Vec<Arc<Cell>>, Ghost(counts): Ghost<Counts>, Ghost(excluded): Ghost<Seq<u8>>, Ghost(pattern): Ghost<Seq<u8>>, Ghost(sizes): Ghost<Seq<i64>>) -> (result: Option<Vec<RuntimeRow>>)
    requires vocab.ok(), cells_source(cells@, counts), cells_selected(cells@, excluded, pattern, sizes),
    ensures result is None ==> !cells_sorted(cells@),
        result matches Some(rows) ==> rows_source(rows_view(rows@), counts)
        && rows_selected(rows_view(rows@), excluded, pattern, sizes) && rows_ordered(rows_view(rows@))
        && rows.len() == cells.len()
        && forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows_view(rows@)[index]) == cell_row(*cells@[index]),
{
    proof { lemma_ext_order(); }
    let mut rows = Vec::with_capacity(cells.len());
    let mut position = 0;
    while position < cells.len()
        invariant position <= cells.len(), rows.len() == position, vocab.ok(),
            cells_source(cells@, counts), rows_source(rows_view(rows@), counts),
            cells_selected(cells@, excluded, pattern, sizes), rows_selected(rows_view(rows@), excluded, pattern, sizes),
            strict_order(ext_order()),
            forall|index: int| 0 <= index < position ==>
                (#[trigger] rows_view(rows@)[index]) == cell_row(*cells@[index]),
            forall|first: int, last: int| 0 <= first < last < position ==>
                !same_group(#[trigger] rows_view(rows@)[first], #[trigger] rows_view(rows@)[last])
                    && row_lt(rows_view(rows@)[first], rows_view(rows@)[last]),
        decreases cells.len() - position,
    {
        let cell = &*cells[position];
        let before = Ghost(rows@);
        if position > 0 && !head_lt(vocab, &*cells[position - 1], cell) {
            proof {
                lemma_row_lt_ext(row_at(cells@, position as int - 1), row_at(cells@, position as int));
                if cells_sorted(cells@) { assert(ext_order()(cell_row(*cells@[position as int - 1]), cell_row(*cells@[position as int]))); }
            }
            return None;
        }
        proof { assert(source_cell(*cell, counts)); assert(cell.size == cell.group.size); }
        rows.push((copy_bytes(brand_bytes(vocab, cell)), copy_bytes(kind_bytes(vocab, cell)), cell.size, cell.count));
        proof {
            assert forall|index: int| 0 <= index < rows.len() implies {
                let row = #[trigger] rows_view(rows@)[index];
                row.supplier_cnt > 0 && row.supplier_cnt == count_at(counts, row_key(row))
            } by {
                if index < position { assert(rows_view(rows@)[index] == rows_view(before@)[index]); }
                else { assert(row_key(rows_view(rows@)[index]) == cell.group.key()); }
            }
            assert(rows_view(rows@)[position as int] == cell_row(*cell));
            assert forall|index: int| 0 <= index < rows.len() implies {
                let row = #[trigger] rows_view(rows@)[index];
                row.p_brand != excluded && !like_match(row.p_type, pattern) && seq_view_i64(sizes).contains(row.p_size)
            } by {
                if index < position { assert(rows_view(rows@)[index] == rows_view(before@)[index]); }
                else {
                    assert(cell_selected(*cell, excluded, pattern, sizes));
                    let selected = choose|selected: int| 0 <= selected < sizes.len()
                        && (#[trigger] sizes[selected]) == cell.group.size;
                    assert(seq_view_i64(sizes)[selected] == cell.group.size as int);
                }
            }
            assert forall|index: int| 0 <= index < rows.len() implies
                (#[trigger] rows_view(rows@)[index]) == cell_row(*cells@[index]) by {
                if index < position { assert(rows_view(rows@)[index] == rows_view(before@)[index]); }
            }
            let current = rows_view(rows@)[position as int];
            assert forall|earlier: int| 0 <= earlier < position implies
                !same_group(#[trigger] rows_view(rows@)[earlier], current)
                    && row_lt(rows_view(rows@)[earlier], current) by {
                if earlier < position as int - 1 {
                    let middle = rows_view(rows@)[position as int - 1];
                    assert(rows_view(rows@)[earlier] == rows_view(before@)[earlier]);
                    assert(middle == rows_view(before@)[position as int - 1]);
                    lemma_row_lt_ext(rows_view(rows@)[earlier], middle);
                    lemma_row_lt_ext(middle, current);
                    lemma_row_lt_ext(rows_view(rows@)[earlier], current);
                }
                if same_group(rows_view(rows@)[earlier], current) {
                    assert(rows_view(rows@)[earlier].supplier_cnt == current.supplier_cnt);
                    lemma_bytes_lt_irrefl(current.p_brand);
                    lemma_bytes_lt_irrefl(current.p_type);
                }
            }
            assert forall|first: int, last: int| 0 <= first < last < rows.len() implies
                !same_group(#[trigger] rows_view(rows@)[first], #[trigger] rows_view(rows@)[last])
                    && row_lt(rows_view(rows@)[first], rows_view(rows@)[last]) by {
                if last < position {
                    assert(rows_view(rows@)[first] == rows_view(before@)[first]);
                    assert(rows_view(rows@)[last] == rows_view(before@)[last]);
                }
            }
        }
        position += 1;
    }
    proof {
        assert forall|first: int, last: int| 0 <= first < last < rows.len() implies
            !same_group(#[trigger] rows_view(rows@)[first], #[trigger] rows_view(rows@)[last])
                && !row_lt(rows_view(rows@)[last], rows_view(rows@)[first]) by {
            lemma_row_lt_ext(rows_view(rows@)[first], rows_view(rows@)[last]);
            lemma_row_lt_ext(rows_view(rows@)[last], rows_view(rows@)[first]);
        }
    }
    Some(rows)
}

pub proof fn lemma_output_model(counts: Counts, ranks: Ranks, rows: Seq<OutRow>, cells: Seq<Arc<Cell>>, excluded: Seq<u8>, pattern: Seq<u8>, sizes: Seq<i64>)
    requires ranks_valid(ranks, counts), sizes.len() == 8, cells_cover(cells, counts, excluded, pattern, sizes),
        rows_source(rows, counts), rows_selected(rows, excluded, pattern, sizes), rows_ordered(rows),
        rows.len() == cells.len(), forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]) == cell_row(*cells[index]),
        cells_source(cells, counts),
    ensures output_model(counts,
        params(excluded, pattern, sizes[0] as int, sizes[1] as int, sizes[2] as int, sizes[3] as int,
            sizes[4] as int, sizes[5] as int, sizes[6] as int, sizes[7] as int), rows),
{
    reveal(rows_source);
    assert forall|index: int| 0 <= index < rows.len() implies {
        let row = #[trigger] rows[index];
        row.p_brand != excluded && !like_match(row.p_type, pattern) && row.supplier_cnt > 0
            && row.supplier_cnt == count_at(counts, row_key(row))
            && (row.p_size == sizes[0] as int || row.p_size == sizes[1] as int
                || row.p_size == sizes[2] as int || row.p_size == sizes[3] as int
                || row.p_size == sizes[4] as int || row.p_size == sizes[5] as int
                || row.p_size == sizes[6] as int || row.p_size == sizes[7] as int)
    } by {
        let row = rows[index];
        assert(seq_view_i64(sizes).contains(row.p_size));
        let selected = choose|selected: int| 0 <= selected < sizes.len()
            && (#[trigger] seq_view_i64(sizes)[selected]) == row.p_size;
        assert(sizes[selected] as int == row.p_size);
    }
    let parameters = params(excluded, pattern, sizes[0] as int, sizes[1] as int, sizes[2] as int, sizes[3] as int,
        sizes[4] as int, sizes[5] as int, sizes[6] as int, sizes[7] as int);
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] count_at(counts, group_key(brand, kind, size)) > 0 && brand != parameters.p1
        && !like_match(kind, parameters.p2)
        && (size == parameters.p3 || size == parameters.p4 || size == parameters.p5 || size == parameters.p6
            || size == parameters.p7 || size == parameters.p8 || size == parameters.p9 || size == parameters.p10) implies
            exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).p_brand == brand
                && rows[index].p_type == kind && rows[index].p_size == size by {
        let key = group_key(brand, kind, size);
        assert(positive_counts(counts));
        let cell = counts.at(key).unwrap();
        assert(cell.group.key() == key);
        lemma_group_text_identity(brand, kind, (*cell.group.brand)@, (*cell.group.kind)@);
        assert(cell.group.size as int == size);
        assert(sizes.contains(cell.group.size)) by {
            let selected = choose|selected: int| 0 <= selected < sizes.len() && sizes[selected] as int == size;
        }
        assert(visible(cell, excluded, pattern));
        assert(cell_has(cells, cell));
        let index = choose|index: int| 0 <= index < cells.len() && *(#[trigger] cells[index]) == cell;
        assert(rows[index] == cell_row(cell));
    }
}


/// The rows as the result type; None when a count does not fit it.
pub fn narrow_rows(rows: &Vec<RuntimeRow>) -> (out: Option<Vec<(Vec<u8>, Vec<u8>, i64, i64)>>)
    ensures out matches Some(narrowed) ==> out_view(narrowed@) == rows_view(rows@),
        out is None ==> exists|index: int| 0 <= index < rows.len() && !(i64::MIN <= (#[trigger] rows@[index]).3 <= i64::MAX),
{
    let mut narrowed: Vec<(Vec<u8>, Vec<u8>, i64, i64)> = Vec::with_capacity(rows.len());
    let mut position = 0;
    while position < rows.len()
        invariant position <= rows.len(), narrowed.len() == position,
            forall|index: int| 0 <= index < position ==> (#[trigger] out_view(narrowed@)[index]) == rows_view(rows@)[index],
        decreases rows.len() - position,
    {
        let row = &rows[position];
        if row.3 < i64::MIN as i128 || row.3 > i64::MAX as i128 { return None; }
        let before = Ghost(narrowed@);
        narrowed.push((copy_bytes(row.0.as_slice()), copy_bytes(row.1.as_slice()), row.2, row.3 as i64));
        proof {
            assert forall|index: int| 0 <= index < position + 1 implies (#[trigger] out_view(narrowed@)[index]) == rows_view(rows@)[index] by {
                if index < position { assert(out_view(narrowed@)[index] == out_view(before@)[index]); }
            }
        }
        position += 1;
    }
    Some(narrowed)
}
}
