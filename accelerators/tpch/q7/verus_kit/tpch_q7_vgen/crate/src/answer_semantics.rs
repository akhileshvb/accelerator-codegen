use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::params;
use crate::storage::*;
use crate::values::*;
use crate::model::*;
use crate::domains::*;
use crate::expansion::*;
use crate::query_proofs::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn cell_nation_weight(key: i64, cell: Cell, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool,
    left: NationRow, right: NationRow) -> int
{
    if cell.year == year && left.n_nationkey == key && left.n_name == first
        && right.n_nationkey == cell.other && right.n_name == second {
        if revenue { cell.revenue as int } else { cell.count as int }
    } else { 0int }
}

pub open spec fn bucket_nation_weight(entry: (i64, Arc<Groups>), first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool,
    left: NationRow, right: NationRow) -> int
{
    if left.n_name == first && right.n_name == second && left.n_nationkey == entry.0 {
        group_at(entry.1.rows@, right.n_nationkey as i64, year, revenue)
    } else { 0int }
}

pub open spec fn named_numeric(db: Db, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow|
        if left.n_name == first && right.n_name == second {
            numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue)
        } else { 0int }))
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_cell_nations(db: Db, nations: Layers<Names>, key: i64, cell: Cell,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires nation_value(nations, key, first) == nation_count(db, key, first),
        nation_value(nations, cell.other, second) == nation_count(db, cell.other, second),
    ensures cell_expansion(nations, key, cell, 1int, first, second, year, revenue)
        == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow|
            cell_nation_weight(key, cell, first, second, year, revenue, left, right))),
{
    reveal(nation_count);
    let measure = if cell.year == year { if revenue { cell.revenue as int } else { cell.count as int } } else { 0int };
    let left_count = |left: NationRow| if left.n_nationkey == key && left.n_name == first { 1int } else { 0int };
    let right_count = |right: NationRow| if right.n_nationkey == cell.other && right.n_name == second { 1int } else { 0int };
    let outer = |left: NationRow| sum(db.nation, |right: NationRow|
        cell_nation_weight(key, cell, first, second, year, revenue, left, right));
    assert forall|left: NationRow| #[trigger] outer(left) == left_count(left) * (measure * nation_count(db, cell.other, second)) by {
        let inner = |right: NationRow| cell_nation_weight(key, cell, first, second, year, revenue, left, right);
        let coefficient = left_count(left) * measure;
        assert forall|right: NationRow| #[trigger] inner(right) == coefficient * right_count(right) by {}
        lemma_sum_scale(db.nation, right_count, inner, coefficient);
        if left.n_nationkey == key && left.n_name == first { assert(coefficient == measure); }
        else { assert(coefficient == 0int); }
    }
    lemma_sum_scale(db.nation, left_count, outer, measure * nation_count(db, cell.other, second));
    assert((measure * nation_count(db, cell.other, second)) * nation_count(db, key, first)
        == nation_count(db, key, first) * (nation_count(db, cell.other, second) * measure)) by (nonlinear_arith);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_bucket_nations(db: Db, nations: Layers<Names>, entry: (i64, Arc<Groups>),
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires database_domain(db),
        forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
            nation_value(nations, key, name) == nation_count(db, key, name),
    ensures sum(entry.1.rows@, expansion_weight(nations, entry.0, first, second, year, revenue))
        == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow|
            bucket_nation_weight(entry, first, second, year, revenue, left, right))),
{
    let weight = |cell: Cell, left: NationRow, right: NationRow|
        cell_nation_weight(entry.0, cell, first, second, year, revenue, left, right);
    let original = expansion_weight(nations, entry.0, first, second, year, revenue);
    let expanded = |cell: Cell| sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| weight(cell, left, right)));
    assert forall|cell: Cell| #[trigger] original(cell) == expanded(cell) by {
        lemma_cell_nations(db, nations, entry.0, cell, first, second, year, revenue);
        let folded = |left: NationRow| sum(db.nation, |right: NationRow| weight(cell, left, right));
        let target = |left: NationRow| sum(db.nation, |right: NationRow| cell_nation_weight(entry.0, cell, first, second, year, revenue, left, right));
        assert((|left: NationRow| sum(db.nation, |right: NationRow| weight(cell, left, right))) =~=
            (|left: NationRow| sum(db.nation, |right: NationRow| cell_nation_weight(entry.0, cell, first, second, year, revenue, left, right)))) by {
            assert forall|left: NationRow| #[trigger] folded(left) == target(left) by {
                assert((|right: NationRow| weight(cell, left, right)) =~= (|right: NationRow|
                    cell_nation_weight(entry.0, cell, first, second, year, revenue, left, right)));
            }
        }
    }
    lemma_sum_ext_all(entry.1.rows@, original, expanded);
    lemma_sum_rotate_three(entry.1.rows@, db.nation, db.nation, weight);
    let transposed = |left: NationRow| sum(db.nation, |right: NationRow| sum(entry.1.rows@, |cell: Cell| weight(cell, left, right)));
    let result = |left: NationRow| sum(db.nation, |right: NationRow| bucket_nation_weight(entry, first, second, year, revenue, left, right));
    assert forall|left_index: int| 0 <= left_index < db.nation.len() implies transposed(#[trigger] db.nation[left_index]) == result(db.nation[left_index]) by {
        let left = db.nation[left_index];
        assert forall|right_index: int| 0 <= right_index < db.nation.len() implies
            sum(entry.1.rows@, |cell: Cell| weight(cell, left, #[trigger] db.nation[right_index]))
                == bucket_nation_weight(entry, first, second, year, revenue, left, db.nation[right_index]) by {
            let right = db.nation[right_index];
            assert(right.n_nationkey as i64 as int == right.n_nationkey);
            if left.n_name == first && right.n_name == second && left.n_nationkey == entry.0 {
                lemma_sum_ext_all(entry.1.rows@, |cell: Cell| weight(cell, left, right), group_weight(right.n_nationkey as i64, year, revenue));
            } else {
                lemma_sum_zero(entry.1.rows@, |cell: Cell| weight(cell, left, right));
            }
        }
        lemma_sum_ext(db.nation, |right: NationRow| sum(entry.1.rows@, |cell: Cell| weight(cell, left, right)),
            |right: NationRow| bucket_nation_weight(entry, first, second, year, revenue, left, right));
    }
    lemma_sum_ext(db.nation, transposed, result);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_sparse_group_fold(groups: Sparse<Groups>, first: i64, second: i64, year: i64, revenue: bool)
    requires groups.valid(),
    ensures sparse_group(groups, first, second, year, revenue) == sum(groups.view(), |entry: (i64, Arc<Groups>)|
        if entry.0 == first { group_at(entry.1.rows@, second, year, revenue) } else { 0int }),
{
    groups.row_facts(); groups.unique_keys();
    let weight = |entry: (i64, Arc<Groups>)| if entry.0 == first { group_at(entry.1.rows@, second, year, revenue) } else { 0int };
    let keys = groups.view().map_values(|entry: (i64, Arc<Groups>)| entry.0);
    if groups.at(first).is_some() {
        let offset = choose|offset: int| #![trigger groups.view()[offset]]
            0 <= offset < groups.view().len() && groups.view()[offset].0 == first && groups.at(first) == Some(*groups.view()[offset].1);
        assert forall|index: int| 0 <= index < groups.view().len() && index != offset implies weight(#[trigger] groups.view()[index]) == 0 by {
            assert(keys[index] == groups.view()[index].0);
            assert(keys[offset] == first);
        }
        lemma_sum_single_match(groups.view(), weight, offset);
    } else {
        assert forall|index: int| 0 <= index < groups.view().len() implies weight(#[trigger] groups.view()[index]) == 0 by {}
        lemma_sum_zero(groups.view(), weight);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_expansion_named(db: Db, groups: Sparse<Groups>, nations: Layers<Names>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires database_domain(db), groups.valid(),
        forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
            nation_value(nations, key, name) == nation_count(db, key, name),
        forall|left: i64, right: i64, cell_year: i64, measure: bool| #![trigger sparse_group(groups, left, right, cell_year, measure)]
            sparse_group(groups, left, right, cell_year, measure) == numeric_group(db, left, right, cell_year, measure),
    ensures expansion(groups.view(), nations, first, second, year, revenue) == named_numeric(db, first, second, year, revenue),
{
    let weight = |entry: (i64, Arc<Groups>), left: NationRow, right: NationRow|
        bucket_nation_weight(entry, first, second, year, revenue, left, right);
    let original = bucket_expansion_weight(nations, first, second, year, revenue);
    let expanded = |entry: (i64, Arc<Groups>)| sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| weight(entry, left, right)));
    assert forall|entry: (i64, Arc<Groups>)| #[trigger] original(entry) == expanded(entry) by {
        lemma_bucket_nations(db, nations, entry, first, second, year, revenue);
        let folded = |left: NationRow| sum(db.nation, |right: NationRow| weight(entry, left, right));
        let target = |left: NationRow| sum(db.nation, |right: NationRow| bucket_nation_weight(entry, first, second, year, revenue, left, right));
        assert((|left: NationRow| sum(db.nation, |right: NationRow| weight(entry, left, right))) =~=
            (|left: NationRow| sum(db.nation, |right: NationRow| bucket_nation_weight(entry, first, second, year, revenue, left, right)))) by {
            assert forall|left: NationRow| #[trigger] folded(left) == target(left) by {
                assert((|right: NationRow| weight(entry, left, right)) =~= (|right: NationRow|
                    bucket_nation_weight(entry, first, second, year, revenue, left, right)));
            }
        }
    }
    lemma_sum_ext_all(groups.view(), original, expanded);
    lemma_sum_rotate_three(groups.view(), db.nation, db.nation, weight);
    let transposed = |left: NationRow| sum(db.nation, |right: NationRow| sum(groups.view(), |entry: (i64, Arc<Groups>)| weight(entry, left, right)));
    let result = |left: NationRow| sum(db.nation, |right: NationRow|
        if left.n_name == first && right.n_name == second { numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int });
    assert forall|left_index: int| 0 <= left_index < db.nation.len() implies transposed(#[trigger] db.nation[left_index]) == result(db.nation[left_index]) by {
        let left = db.nation[left_index];
        assert(left.n_nationkey as i64 as int == left.n_nationkey);
        let inner = |right: NationRow| sum(groups.view(), |entry: (i64, Arc<Groups>)| weight(entry, left, right));
        let target = |right: NationRow| if left.n_name == first && right.n_name == second {
            numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int };
        assert forall|right: NationRow| #[trigger] inner(right) == target(right) by {
            if left.n_name == first && right.n_name == second {
                lemma_sparse_group_fold(groups, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue);
                lemma_sum_ext_all(groups.view(), |entry: (i64, Arc<Groups>)| weight(entry, left, right),
                    |entry: (i64, Arc<Groups>)| if entry.0 == left.n_nationkey as i64 { group_at(entry.1.rows@, right.n_nationkey as i64, year, revenue) } else { 0int });
            } else { lemma_sum_zero(groups.view(), |entry: (i64, Arc<Groups>)| weight(entry, left, right)); }
        }
        lemma_sum_ext_all(db.nation, inner, target);
    }
    lemma_sum_ext(db.nation, transposed, result);
}

}
