use vstd::prelude::*;
use verus_common::order::*;
use crate::gen_spec::*;

verus! {

pub open spec fn bounded_insert(rows: Seq<OutRow>, row: OutRow) -> Seq<OutRow> {
    limit_rows(insert_by(rows, row, ext_order()), 100)
}

pub open spec fn repeat_retained(rows: Seq<OutRow>, row: OutRow, copies: nat) -> Seq<OutRow>
    decreases copies,
{
    if copies == 0 { rows }
    else { bounded_insert(repeat_retained(rows, row, (copies - 1) as nat), row) }
}

pub open spec fn repeat_sorted(rows: Seq<OutRow>, row: OutRow, copies: nat) -> Seq<OutRow>
    decreases copies,
{
    if copies == 0 { rows }
    else { insert_by(repeat_sorted(rows, row, (copies - 1) as nat), row, ext_order()) }
}

pub proof fn lemma_limit_prepend(row: OutRow, rows: Seq<OutRow>, limit: nat)
    ensures limit_rows(seq![row] + rows, limit as int) ==
        if limit == 0 { Seq::<OutRow>::empty() } else { seq![row] + limit_rows(rows, limit as int - 1) },
{
    if limit == 0 { assert((seq![row] + rows).take(0) =~= Seq::<OutRow>::empty()); }
    else if rows.len() >= limit {
        assert((seq![row] + rows).take(limit as int) =~= seq![row] + rows.take(limit as int - 1));
    }
}

pub proof fn lemma_limit_nested(rows: Seq<OutRow>, small: nat, large: nat)
    requires small <= large,
    ensures limit_rows(limit_rows(rows, large as int), small as int) == limit_rows(rows, small as int),
{
    if rows.len() > large {
        assert(rows.take(large as int).take(small as int) =~= rows.take(small as int));
    }
}

pub proof fn lemma_insert_limit(rows: Seq<OutRow>, row: OutRow, limit: nat)
    ensures limit_rows(insert_by(rows, row, ext_order()), limit as int) ==
        limit_rows(insert_by(limit_rows(rows, limit as int), row, ext_order()), limit as int),
    decreases limit,
{
    if limit == 0 {
        assert(limit_rows(insert_by(rows, row, ext_order()), 0) =~= Seq::<OutRow>::empty());
        assert(limit_rows(rows, 0) =~= Seq::<OutRow>::empty());
    } else if rows.len() > 0 {
        let prefix = limit_rows(rows, limit as int);
        let tail = rows.drop_first();
        assert(rows =~= seq![rows[0]] + tail);
        lemma_limit_prepend(rows[0], tail, limit);
        assert(prefix[0] == rows[0]);
        assert(prefix.drop_first() =~= limit_rows(tail, limit as int - 1));
        if ext_lt(row, rows[0]) {
            lemma_limit_prepend(row, rows, limit);
            lemma_limit_prepend(row, prefix, limit);
            lemma_limit_nested(rows, (limit - 1) as nat, limit);
        } else {
            lemma_insert_limit(tail, row, (limit - 1) as nat);
            lemma_limit_prepend(rows[0], insert_by(tail, row, ext_order()), limit);
            lemma_limit_prepend(rows[0], insert_by(prefix.drop_first(), row, ext_order()), limit);
        }
    }
}

pub proof fn lemma_repeat_prefix(rows: Seq<OutRow>, row: OutRow, copies: nat)
    ensures repeat_retained(limit_rows(rows, 100), row, copies) == limit_rows(repeat_sorted(rows, row, copies), 100),
    decreases copies,
{
    if copies > 0 {
        lemma_repeat_prefix(rows, row, (copies - 1) as nat);
        lemma_insert_limit(repeat_sorted(rows, row, (copies - 1) as nat), row, 100);
    }
}

pub proof fn lemma_repeat_sorted(rows: Seq<OutRow>, row: OutRow, copies: nat)
    requires nondesc(rows, ext_order()),
    ensures nondesc(repeat_sorted(rows, row, copies), ext_order()),
        repeat_sorted(rows, row, copies).to_multiset().count(row) == rows.to_multiset().count(row) + copies,
        forall|other: OutRow| other != row ==> #[trigger] repeat_sorted(rows, row, copies).to_multiset().count(other) == rows.to_multiset().count(other),
    decreases copies,
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    if copies > 0 {
        lemma_repeat_sorted(rows, row, (copies - 1) as nat);
        let previous = repeat_sorted(rows, row, (copies - 1) as nat);
        lemma_ext_order();
        assert forall|other: OutRow| previous.contains(other) && other != row implies #[trigger] ext_order()(row, other) || #[trigger] ext_order()(other, row) by {
            lemma_ext_total(row, other);
        }
        lemma_insert_by_dups(previous, row, ext_order());
    }
}

pub proof fn lemma_count_bound(rows: Seq<OutRow>, row: OutRow)
    ensures rows.to_multiset().count(row) <= rows.len(),
    decreases rows.len(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    if rows.len() > 0 {
        lemma_count_bound(rows.drop_last(), row);
        assert(rows =~= rows.drop_last().push(rows.last()));
    }
}

pub proof fn lemma_count_prefix(rows: Seq<OutRow>, row: OutRow, prefix: nat)
    requires prefix <= rows.len(),
        forall|index: int| prefix <= index < rows.len() ==> #[trigger] rows[index] != row,
    ensures rows.to_multiset().count(row) == rows.take(prefix as int).to_multiset().count(row),
    decreases rows.len() - prefix,
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    if rows.len() > prefix {
        assert(rows.last() != row);
        assert forall|index: int| prefix <= index < rows.drop_last().len() implies #[trigger] rows.drop_last()[index] != row by {
            assert(rows.drop_last()[index] == rows[index]);
        }
        lemma_count_prefix(rows.drop_last(), row, prefix);
        assert(rows =~= rows.drop_last().push(rows.last()));
        assert(rows.drop_last().take(prefix as int) =~= rows.take(prefix as int));
    } else { assert(rows.take(prefix as int) =~= rows); }
}

pub proof fn lemma_prefix_saturated(rows: Seq<OutRow>, row: OutRow)
    requires nondesc(rows, ext_order()), rows.to_multiset().count(row) >= 100,
    ensures rows.len() >= 100, !ext_lt(row, rows[99]),
{
    lemma_ext_order();
    lemma_count_bound(rows, row);
    if ext_lt(row, rows[99]) {
        assert forall|index: int| 99 <= index < rows.len() implies #[trigger] rows[index] != row by {
            if index == 99 { assert(!ext_order()(row, row)); }
            else { assert(!ext_order()(rows[index], rows[99])); }
        }
        lemma_count_prefix(rows, row, 99);
        lemma_count_bound(rows.take(99), row);
    }
}

pub proof fn lemma_repeat_capped(rows: Seq<OutRow>, row: OutRow, copies: nat)
    requires rows.len() <= 100, nondesc(rows, ext_order()),
    ensures repeat_retained(rows, row, copies) == repeat_retained(rows, row, if copies >= 100 { 100nat } else { copies }),
{
    if copies >= 100 {
        lemma_repeat_sorted(rows, row, 100);
        let full = repeat_sorted(rows, row, 100);
        lemma_prefix_saturated(full, row);
        let prefix = limit_rows(full, 100);
        assert(prefix.len() == 100);
        assert(prefix[99] == full[99]);
        assert(nondesc(prefix, ext_order()));
        lemma_reject_limited(prefix, row);
        lemma_repeat_unchanged(prefix, row, (copies - 100) as nat);
        lemma_repeat_prefix(rows, row, 100);
        lemma_repeat_join(rows, row, 100, (copies - 100) as nat);
    }
}

pub open spec fn cap_multiplicity(value: int) -> int { if value >= 100 { 100int } else { value } }

pub open spec fn copies_of(row: OutRow, copies: nat) -> Seq<OutRow> {
    Seq::new(copies, |position: int| row)
}

pub proof fn lemma_copies_bag(row: OutRow, copies: nat)
    ensures copies_of(row, copies).to_multiset().count(row) == copies,
        forall|other: OutRow| other != row ==> #[trigger] copies_of(row, copies).to_multiset().count(other) == 0,
    decreases copies,
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    if copies > 0 {
        lemma_copies_bag(row, (copies - 1) as nat);
        assert(copies_of(row, copies) =~= copies_of(row, (copies - 1) as nat).push(row));
    } else { assert(copies_of(row, copies) =~= Seq::<OutRow>::empty()); }
}

pub proof fn lemma_repeat_append(rows: Seq<OutRow>, row: OutRow, copies: nat)
    requires nondesc(rows, ext_order()),
    ensures nondesc(repeat_sorted(rows, row, copies), ext_order()),
        repeat_sorted(rows, row, copies).to_multiset() == (rows + copies_of(row, copies)).to_multiset(),
{
    broadcast use vstd::multiset::group_multiset_axioms;
    lemma_repeat_sorted(rows, row, copies);
    lemma_copies_bag(row, copies);
    verus_common::sums::lemma_to_multiset_concat(rows, copies_of(row, copies));
    assert(repeat_sorted(rows, row, copies).to_multiset() =~= (rows + copies_of(row, copies)).to_multiset()) by {
        assert forall|other: OutRow| #[trigger] repeat_sorted(rows, row, copies).to_multiset().count(other) ==
            (rows + copies_of(row, copies)).to_multiset().count(other) by {}
    }
}

pub proof fn lemma_cap_multiply(left: int, right: int)
    requires left >= 0, right >= 0,
    ensures cap_multiplicity(cap_multiplicity(left) * right) == cap_multiplicity(left * right),
{
    if left >= 100 && right > 0 {
        assert(left * right >= 100) by (nonlinear_arith) requires left >= 100, right > 0;
        assert(100int * right >= 100) by (nonlinear_arith) requires right > 0;
    }
}

pub proof fn lemma_retained_product(full: Seq<OutRow>, row: OutRow, copies: nat, best: Seq<OutRow>, result: Seq<OutRow>)
    requires nondesc(full, ext_order()), best == limit_rows(full, 100),
        result == repeat_retained(best, row, (cap_multiplicity(copies as int)) as nat),
    ensures result == limit_rows(repeat_sorted(full, row, copies), 100), nondesc(repeat_sorted(full, row, copies), ext_order()),
{
    assert(nondesc(best, ext_order()));
    lemma_repeat_capped(best, row, copies);
    lemma_repeat_prefix(full, row, copies);
    lemma_repeat_sorted(full, row, copies);
}

pub proof fn lemma_repeat_join(rows: Seq<OutRow>, row: OutRow, first: nat, second: nat)
    ensures repeat_retained(rows, row, first + second) == repeat_retained(repeat_retained(rows, row, first), row, second),
    decreases second,
{
    if second > 0 { lemma_repeat_join(rows, row, first, (second - 1) as nat); }
}

pub proof fn lemma_repeat_unchanged(rows: Seq<OutRow>, row: OutRow, copies: nat)
    requires bounded_insert(rows, row) == rows,
    ensures repeat_retained(rows, row, copies) == rows,
    decreases copies,
{
    if copies > 0 { lemma_repeat_unchanged(rows, row, (copies - 1) as nat); }
}

pub proof fn lemma_reject_limited(rows: Seq<OutRow>, row: OutRow)
    requires rows.len() == 100, nondesc(rows, ext_order()), !ext_lt(row, rows[99]),
    ensures bounded_insert(rows, row) == rows,
{
    lemma_ext_order();
    assert forall|index: int| 0 <= index < rows.len() implies !ext_lt(row, #[trigger] rows[index]) by {
        if rows[index] != rows[99] {
            lemma_ext_total(rows[index], rows[99]);
            assert(!ext_lt(rows[99], rows[index]));
            assert(ext_lt(rows[index], rows[99]));
            if ext_lt(row, rows[index]) {
                assert(ext_order()(row, rows[index]));
                assert(ext_order()(rows[index], rows[99]));
                assert(ext_order()(row, rows[99]));
            }
        }
    }
    lemma_insert_position(rows, row, rows.len() as int);
    assert(rows.insert(rows.len() as int, row) =~= rows.push(row));
    assert(rows.push(row).take(100) =~= rows);
}

pub proof fn lemma_insert_position(rows: Seq<OutRow>, row: OutRow, position: int)
    requires 0 <= position <= rows.len(),
        forall|index: int| 0 <= index < position ==> !ext_lt(row, #[trigger] rows[index]),
        position < rows.len() ==> ext_lt(row, rows[position]),
    ensures insert_by(rows, row, ext_order()) == rows.insert(position, row),
    decreases position,
{
    if position == 0 {
        assert(rows.insert(position, row) =~= seq![row] + rows);
    } else {
        assert(!ext_lt(row, rows[0]));
        assert forall|index: int| 0 <= index < position - 1 implies !ext_lt(row, #[trigger] rows.drop_first()[index]) by {
            assert(rows.drop_first()[index] == rows[index + 1]);
        }
        if position < rows.len() { assert(rows.drop_first()[position - 1] == rows[position]); }
        lemma_insert_position(rows.drop_first(), row, position - 1);
        assert(rows.insert(position, row) =~= seq![rows[0]] + rows.drop_first().insert(position - 1, row));
    }
}

pub proof fn lemma_ordered_insert(rows: Seq<OutRow>, row: OutRow, position: int)
    requires nondesc(rows, ext_order()), 0 <= position <= rows.len(),
        forall|index: int| 0 <= index < position ==> !ext_lt(row, #[trigger] rows[index]),
        position < rows.len() ==> ext_lt(row, rows[position]),
    ensures nondesc(rows.insert(position, row), ext_order()),
{
    lemma_ext_order();
    assert forall|other: OutRow| rows.contains(other) && other != row implies #[trigger] ext_order()(row, other) || #[trigger] ext_order()(other, row) by {
        lemma_ext_total(row, other);
    }
    lemma_insert_by_dups(rows, row, ext_order());
    lemma_insert_position(rows, row, position);
}

}
