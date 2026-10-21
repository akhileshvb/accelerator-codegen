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

}
