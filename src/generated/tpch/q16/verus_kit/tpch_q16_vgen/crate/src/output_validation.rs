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

pub fn materialize(vocab: &Vocab, cells: &Vec<Arc<Cell>>, Ghost(counts): Ghost<Counts>, Ghost(excluded): Ghost<Seq<u8>>, Ghost(pattern): Ghost<Seq<u8>>, Ghost(sizes): Ghost<Seq<i64>>) -> (result: Option<Vec<RuntimeRow>>)
    requires vocab.ok(), cells_source(cells@, counts), cells_selected(cells@, excluded, pattern, sizes),
    ensures result matches Some(rows) ==> rows_source(out_view(rows@), counts)
        && rows_selected(out_view(rows@), excluded, pattern, sizes) && rows_ordered(out_view(rows@))
        && rows.len() == cells.len()
        && forall|index: int| 0 <= index < rows.len() ==> (#[trigger] out_view(rows@)[index]) == cell_row(*cells@[index]),
{
    proof { lemma_ext_order(); }
    let mut rows = Vec::with_capacity(cells.len());
    let mut position = 0;
    while position < cells.len()
        invariant position <= cells.len(), rows.len() == position, vocab.ok(),
            cells_source(cells@, counts), rows_source(out_view(rows@), counts),
            cells_selected(cells@, excluded, pattern, sizes), rows_selected(out_view(rows@), excluded, pattern, sizes),
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
        if position > 0 && !head_lt(vocab, &*cells[position - 1], cell) { return None; }
        proof { assert(source_cell(*cell, counts)); assert(cell.size == cell.group.size); }
        rows.push((copy_bytes(brand_bytes(vocab, cell)), copy_bytes(kind_bytes(vocab, cell)), cell.size, cell.count));
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
                row.p_brand != excluded && !like_match(row.p_type, pattern) && seq_view_i64(sizes).contains(row.p_size)
            } by {
                if index < position { assert(out_view(rows@)[index] == out_view(before@)[index]); }
                else {
                    assert(cell_selected(*cell, excluded, pattern, sizes));
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

}
