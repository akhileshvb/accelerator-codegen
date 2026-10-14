use vstd::prelude::*;
use verus_prooflib::ordered::KeyView;
use crate::model::*;

verus! {

pub open spec fn cells_valid_rows(rows: Seq<(KeyView, Cell)>) -> bool {
    forall|row: (KeyView, Cell)| #[trigger] rows.contains(row) ==> row.1.group.valid() && row.1.group.key() == row.0
}

pub proof fn lemma_cells_tail(rows: Seq<(KeyView, Cell)>)
    requires cells_valid_rows(rows), rows.len() > 0,
    ensures cells_valid_rows(rows.skip(1)),
{
    assert forall|row: (KeyView, Cell)| #[trigger] rows.skip(1).contains(row) implies
        row.1.group.valid() && row.1.group.key() == row.0 by {
        let position = choose|position: int| 0 <= position < rows.skip(1).len() && #[trigger] rows.skip(1)[position] == row;
        assert(rows[position + 1] == row);
        assert(rows.contains(row));
    }
}

pub proof fn lemma_parts_tail(rows: Seq<(KeyView, Counts)>)
    requires rows.len() > 0,
        forall|row: (KeyView, Counts)| #[trigger] rows.contains(row) ==> row.0.1 == Seq::<u8>::empty() && cell_keys_valid(row.1),
    ensures forall|row: (KeyView, Counts)| #[trigger] rows.skip(1).contains(row) ==> row.0.1 == Seq::<u8>::empty() && cell_keys_valid(row.1),
{
    assert forall|row: (KeyView, Counts)| #[trigger] rows.skip(1).contains(row) implies
        row.0.1 == Seq::<u8>::empty() && cell_keys_valid(row.1) by {
        let position = choose|position: int| 0 <= position < rows.skip(1).len() && #[trigger] rows.skip(1)[position] == row;
        assert(rows[position + 1] == row);
        assert(rows.contains(row));
    }
}

pub proof fn lemma_integer_tail(rows: Seq<(KeyView, i64)>)
    requires rows.len() > 0,
        forall|row: (KeyView, i64)| #[trigger] rows.contains(row) ==> row.0.1 == Seq::<u8>::empty(),
    ensures forall|row: (KeyView, i64)| #[trigger] rows.skip(1).contains(row) ==> row.0.1 == Seq::<u8>::empty(),
{
    assert forall|row: (KeyView, i64)| #[trigger] rows.skip(1).contains(row) implies row.0.1 == Seq::<u8>::empty() by {
        let position = choose|position: int| 0 <= position < rows.skip(1).len() && #[trigger] rows.skip(1)[position] == row;
        assert(rows[position + 1] == row);
        assert(rows.contains(row));
    }
}

}
