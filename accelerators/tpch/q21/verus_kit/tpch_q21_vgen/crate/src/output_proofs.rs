use vstd::prelude::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::aggregates::*;
use crate::index::*;
use crate::output::group_rows;

verus! {

pub proof fn lemma_row_order()
    ensures strict_order(ext_order()),
        forall|first: OutRow, second: OutRow| first != second ==> #[trigger] ext_order()(first, second) || ext_order()(second, first),
{
    lemma_ext_order();
    assert forall|first: OutRow, second: OutRow| first != second implies #[trigger] ext_order()(first, second) || ext_order()(second, first) by {
        if first.numwait == second.numwait {
            assert(first.s_name != second.s_name);
            lemma_ext_total(first, second);
        }
    }
}

pub proof fn lemma_groups_result(db: Db, parameter: Params, groups: &Counts, multiplier: int)
    requires positive(*groups), multiplier > 0,
        forall|position: int| 0 <= position < groups.entries().len() ==> (#[trigger] groups.entries()[position]).0.0 == 0,
        forall|name: Seq<u8>| #![trigger group_count(db, parameter, name)] group_count(db, parameter, name) == multiplier * count(*groups, (0int, name)),
    ensures query_result(db, parameter, limit_rows(sort_by(group_rows(groups.entries(), multiplier), ext_order()), 100)),
{
    lemma_row_order();
    groups.model();
    let rows = group_rows(groups.entries(), multiplier);
    let full = sort_by(rows, ext_order());
    lemma_sort_by(rows, ext_order());
    assert forall|position: int| 0 <= position < full.len() implies row_ok(db, parameter, #[trigger] full[position]) by {
        assert(full.contains(full[position]));
        assert(rows.contains(full[position]));
        let source = choose|source: int| #![auto] 0 <= source < rows.len() && rows[source] == full[position];
        let key = groups.entries()[source].0;
        assert(count(*groups, key) == groups.entries()[source].1 as int);
        assert(count(*groups, key) * multiplier == multiplier * count(*groups, key)) by(nonlinear_arith);
        assert(full[position].numwait == multiplier * count(*groups, key));
        assert(multiplier * count(*groups, key) > 0) by(nonlinear_arith)
            requires multiplier > 0, count(*groups, key) > 0;
    }
    assert forall|first: int, second: int| 0 <= first < second < full.len() implies !same_group(#[trigger] full[first], #[trigger] full[second]) && !row_lt(full[second], full[first]) by {
        if same_group(full[first], full[second]) {
            assert(full[first] == full[second]);
            assert(!ext_order()(full[first], full[first]));
        }
        if row_lt(full[second], full[first]) {
            assert(ext_order()(full[second], full[first]));
            assert(ext_order()(full[first], full[first]));
        }
    }
    assert forall|name: Seq<u8>| #[trigger] group_count(db, parameter, name) > 0 && having(db, parameter, name) implies
        exists|position: int| 0 <= position < full.len() && (#[trigger] full[position]).s_name == name by {
        let key = (0int, name);
        assert(groups.at(key).is_some());
        let source = choose|source: int| #![auto] 0 <= source < groups.entries().len() && groups.entries()[source].0 == key;
        assert(rows.contains(rows[source]));
        assert(full.contains(rows[source]));
        let position = choose|position: int| #![auto] 0 <= position < full.len() && full[position] == rows[source];
    }
    assert(full_result(db, parameter, full));
}

pub proof fn lemma_insert_length(rows: Seq<OutRow>, row: OutRow)
    ensures insert_by(rows, row, ext_order()).len() == rows.len() + 1,
    decreases rows.len(),
{
    if rows.len() > 0 && !ext_order()(row, rows[0]) {
        lemma_insert_length(rows.drop_first(), row);
    }
}

pub proof fn lemma_sort_push(rows: Seq<OutRow>, row: OutRow)
    requires strict_order(ext_order()),
        forall|first: OutRow, second: OutRow| first != second ==> #[trigger] ext_order()(first, second) || ext_order()(second, first),
        !rows.contains(row),
    ensures sort_by(rows.push(row), ext_order()) == insert_by(sort_by(rows, ext_order()), row, ext_order()),
{
    lemma_sort_by(rows, ext_order());
    assert(!sort_by(rows, ext_order()).contains(row));
    assert(rows.push(row).drop_last() =~= rows);
    reveal_with_fuel(sort_by, 2);
}

pub proof fn lemma_insert_position(rows: Seq<OutRow>, row: OutRow, position: int)
    requires
        0 <= position <= rows.len(),
        forall|index: int| 0 <= index < position ==> !ext_order()(row, #[trigger] rows[index]),
        position < rows.len() ==> ext_order()(row, rows[position]),
    ensures insert_by(rows, row, ext_order()) == rows.take(position) + seq![row] + rows.skip(position),
    decreases position,
{
    if position == 0 {
        assert(rows.take(0) =~= Seq::<OutRow>::empty());
        assert(rows.skip(0) =~= rows);
    } else {
        let tail = rows.drop_first();
        assert forall|index: int| 0 <= index < position - 1 implies !ext_order()(row, #[trigger] tail[index]) by {
            assert(tail[index] == rows[index + 1]);
        }
        lemma_insert_position(tail, row, position - 1);
        assert(rows.take(position) =~= seq![rows[0]] + tail.take(position - 1));
        assert(rows.skip(position) =~= tail.skip(position - 1));
    }
}

pub proof fn lemma_limit_insert(rows: Seq<OutRow>, row: OutRow, limit: int)
    requires limit >= 0,
    ensures limit_rows(insert_by(rows, row, ext_order()), limit) ==
        limit_rows(insert_by(limit_rows(rows, limit), row, ext_order()), limit),
    decreases limit,
{
    lemma_insert_length(rows, row);
    lemma_insert_length(limit_rows(rows, limit), row);
    if limit == 0 {
        assert(limit_rows(insert_by(rows, row, ext_order()), limit) =~= Seq::<OutRow>::empty());
        assert(limit_rows(insert_by(limit_rows(rows, limit), row, ext_order()), limit) =~= Seq::<OutRow>::empty());
    } else if rows.len() > limit {
        let prefix = rows.take(limit);
        assert(prefix[0] == rows[0]);
        if ext_order()(row, rows[0]) {
            assert(limit_rows(insert_by(rows, row, ext_order()), limit) =~= seq![row] + rows.take(limit - 1));
            assert(limit_rows(insert_by(prefix, row, ext_order()), limit) =~= seq![row] + rows.take(limit - 1));
        } else {
            let tail = rows.drop_first();
            lemma_limit_insert(tail, row, limit - 1);
            assert(prefix.drop_first() =~= limit_rows(tail, limit - 1));
            assert(limit_rows(insert_by(rows, row, ext_order()), limit) =~= seq![rows[0]] + limit_rows(insert_by(tail, row, ext_order()), limit - 1));
            assert(limit_rows(insert_by(prefix, row, ext_order()), limit) =~= seq![rows[0]] + limit_rows(insert_by(prefix.drop_first(), row, ext_order()), limit - 1));
        }
    }
}

}
