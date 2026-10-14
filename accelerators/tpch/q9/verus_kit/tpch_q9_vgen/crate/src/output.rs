use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::storage::*;
use crate::meaning::*;
use crate::values::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn output_key(row: OutRow) -> (Seq<u8>, int) { (row.nation, row.o_year) }

pub open spec fn row_view(row: (Vec<u8>, i64, i128)) -> OutRow {
    OutRow { nation: row.0@, o_year: row.1 as int, sum_profit: row.2 as int }
}

pub proof fn lemma_row_preorder(first: OutRow, middle: OutRow, last: OutRow)
    requires !row_lt(middle, first), !row_lt(last, middle),
    ensures !row_lt(last, first),
{
    lemma_ext_order();
    if row_lt(last, first) {
        if !same_group(first, middle) {
            lemma_ext_total(first, middle);
            assert(ext_order()(first, middle));
            assert(ext_order()(last, first));
            assert(ext_order()(last, middle));
        } else { assert(row_lt(last, middle)); }
    }
}

pub proof fn lemma_index_permutation(sorted: Seq<usize>, original: Seq<usize>, length: nat)
    requires verus_common::sort::same_bag(sorted, original), original.len() == length,
        forall|index: int| 0 <= index < original.len() ==> (#[trigger] original[index]) as int == index,
        forall|index: int| 0 <= index < sorted.len() ==> (#[trigger] sorted[index]) < length,
    ensures sorted.no_duplicates(),
        forall|index: usize| index < length ==> #[trigger] sorted.contains(index),
{
    assert forall|left: int, right: int| 0 <= left < right < sorted.len() implies
        (#[trigger] sorted[left]) != (#[trigger] sorted[right]) by {
        if sorted[left] == sorted[right] {
            let target = sorted[left];
            let weight = |item: usize| if item == target { 1int } else { 0int };
            lemma_sum_single_match(original, weight, target as int);
            assert(sum(sorted, weight) == sum(original, weight));
            lemma_sum_remove(sorted, left, weight);
            let smaller = sorted.remove(left);
            assert(smaller[right - 1] == target);
            lemma_sum_ge_term(smaller, weight, right - 1);
        }
    }
    assert forall|index: usize| index < length implies #[trigger] sorted.contains(index) by {
        let weight = |item: usize| if item == index { 1int } else { 0int };
        lemma_sum_single_match(original, weight, index as int);
        assert(sum(sorted, weight) == 1);
        if !sorted.contains(index) {
            assert forall|position: int| 0 <= position < sorted.len() implies weight(#[trigger] sorted[position]) == 0 by {}
            lemma_sum_zero(sorted, weight);
        }
    }
}

pub open spec fn group_rows(group: Group) -> Seq<OutRow> {
    group.totals@.map_values(|total: Total| OutRow { nation: group.name@, o_year: total.year as int, sum_profit: total.profit as int })
}

pub open spec fn bucket_rows(groups: Seq<Group>) -> Seq<OutRow> {
    flat(groups.map_values(|group: Group| group_rows(group)))
}

pub open spec fn cache_rows(state: Pages<Vec<Group>>, keys: Seq<i64>) -> Seq<OutRow> {
    flat(keys.map_values(|key: i64| cache_page(state, key)))
}

pub open spec fn cache_page(state: Pages<Vec<Group>>, key: i64) -> Seq<OutRow> {
    match state.at(key) { Some(groups) => bucket_rows(groups@), None => Seq::empty() }
}

pub open spec fn groups_shape(groups: Pages<Vec<Group>>) -> bool {
    groups.valid() && forall|key: i64| #[trigger] groups.at(key).is_some() ==> {
        let groups = groups.at(key).unwrap();
        groups.len() > 0 && groups@.map_values(|group: Group| group.name@).no_duplicates()
            && forall|index: int| 0 <= index < groups.len() ==> {
                let group = #[trigger] groups[index];
                name_hash(group.name@) == key as int && group.totals.len() > 0 && totals_valid(group.totals@)
            }
    }
}

pub open spec fn bucket_valid(groups: Seq<Group>) -> bool {
    groups.map_values(|group: Group| group.name@).no_duplicates()
        && forall|index: int| 0 <= index < groups.len() ==> totals_valid((#[trigger] groups[index]).totals@)
}

pub proof fn lemma_bucket_member(groups: Seq<Group>, row: OutRow)
    requires bucket_rows(groups).contains(row),
    ensures exists|group_index: int, total_index: int| 0 <= group_index < groups.len()
        && 0 <= total_index < groups[group_index].totals.len()
        && #[trigger] group_rows(groups[group_index])[total_index] == row,
{
    let pages = groups.map_values(|group: Group| group_rows(group));
    lemma_flat_contains(pages, row);
    let group_index = choose|index: int| 0 <= index < pages.len() && (#[trigger] pages[index]).contains(row);
    assert(pages[group_index] == group_rows(groups[group_index]));
    let total_index = choose|index: int| 0 <= index < group_rows(groups[group_index]).len() && (#[trigger] group_rows(groups[group_index]))[index] == row;
}

pub proof fn lemma_bucket_cell(groups: Seq<Group>, group_index: int, total_index: int)
    requires bucket_valid(groups), 0 <= group_index < groups.len(), 0 <= total_index < groups[group_index].totals.len(),
    ensures groups_at(groups, groups[group_index].name@, groups[group_index].totals[total_index].year as int, 0)
            == groups[group_index].totals[total_index].count as int,
        groups_at(groups, groups[group_index].name@, groups[group_index].totals[total_index].year as int, 1)
            == groups[group_index].totals[total_index].profit as int,
{
    let group = groups[group_index];
    let year = group.totals[total_index].year as int;
    assert(totals_valid(group.totals@));
    assert forall|index: int| 0 <= index < group.totals.len() && index != total_index implies
        (#[trigger] group.totals[index]).year as int != year by {
        assert(group.totals@.map_values(|row: Total| row.year)[index] == group.totals[index].year);
        assert(group.totals@.map_values(|row: Total| row.year)[total_index] == group.totals[total_index].year);
    }
    lemma_sum_single_match(group.totals@, |row: Total| if row.year == year { if 0int == 0 { row.count as int } else { row.profit as int } } else { 0 }, total_index);
    lemma_sum_single_match(group.totals@, |row: Total| if row.year == year { if 1int == 0 { row.count as int } else { row.profit as int } } else { 0 }, total_index);
    assert(total_at(group.totals@, year, 0) == group.totals[total_index].count as int);
    assert(total_at(group.totals@, year, 1) == group.totals[total_index].profit as int);
    assert forall|index: int| 0 <= index < groups.len() && index != group_index implies
        (#[trigger] groups[index]).name@ != group.name@ by {
        assert(groups.map_values(|row: Group| row.name@)[index] == groups[index].name@);
        assert(groups.map_values(|row: Group| row.name@)[group_index] == group.name@);
    }
    lemma_sum_single_match(groups, group_weight(group.name@, year, 0), group_index);
    lemma_sum_single_match(groups, group_weight(group.name@, year, 1), group_index);
}

pub proof fn lemma_bucket_unique(groups: Seq<Group>)
    requires bucket_valid(groups),
    ensures bucket_rows(groups).map_values(|row: OutRow| output_key(row)).no_duplicates(),
{
    let pages = groups.map_values(|group: Group| group_rows(group));
    let key_pages = pages.map_values(|page: Seq<OutRow>| page.map_values(|row: OutRow| output_key(row)));
    verus_prooflib::lemma_map_flat(pages, |row: OutRow| output_key(row));
    assert forall|position: int| 0 <= position < key_pages.len() implies (#[trigger] key_pages[position]).no_duplicates() by {
        let group = groups[position];
        assert(totals_valid(group.totals@));
        assert forall|left: int, right: int| 0 <= left < right < key_pages[position].len() implies
            (#[trigger] key_pages[position][left]) != (#[trigger] key_pages[position][right]) by {
            assert(group.totals@.map_values(|row: Total| row.year)[left] != group.totals@.map_values(|row: Total| row.year)[right]);
        }
    }
    assert forall|left: int, right: int, key: (Seq<u8>, int)| #![trigger key_pages[left].contains(key), key_pages[right].contains(key)]
        0 <= left < right < key_pages.len() implies !(key_pages[left].contains(key) && key_pages[right].contains(key)) by {
        if key_pages[left].contains(key) && key_pages[right].contains(key) {
            let left_index = choose|index: int| 0 <= index < key_pages[left].len() && (#[trigger] key_pages[left][index]) == key;
            let right_index = choose|index: int| 0 <= index < key_pages[right].len() && (#[trigger] key_pages[right][index]) == key;
            assert(groups[left].name@ == key.0 && groups[right].name@ == key.0);
            assert(groups.map_values(|group: Group| group.name@)[left] != groups.map_values(|group: Group| group.name@)[right]);
        }
    }
    verus_prooflib::lemma_flat_unique(key_pages);
}

pub proof fn lemma_bucket_support(groups: Seq<Group>, name: Seq<u8>, year: int, field: int)
    requires groups_at(groups, name, year, field) != 0,
    ensures exists|row: OutRow| #[trigger] bucket_rows(groups).contains(row) && row.nation == name && row.o_year == year,
{
    if !(exists|row: OutRow| #[trigger] bucket_rows(groups).contains(row) && row.nation == name && row.o_year == year) {
        let pages = groups.map_values(|group: Group| group_rows(group));
        assert forall|group_index: int| 0 <= group_index < groups.len() implies
            (if (#[trigger] groups[group_index]).name@ == name { total_at(groups[group_index].totals@, year, field) } else { 0 }) == 0 by {
            let group = groups[group_index];
            if group.name@ == name {
                assert forall|total_index: int| 0 <= total_index < group.totals.len() implies
                    (if (#[trigger] group.totals[total_index]).year as int == year { if field == 0 { group.totals[total_index].count as int } else { group.totals[total_index].profit as int } } else { 0 }) == 0 by {
                    if group.totals[total_index].year as int == year {
                        let row = group_rows(group)[total_index];
                        assert(pages[group_index].contains(row));
                        lemma_flat_contains(pages, row);
                        assert(bucket_rows(groups).contains(row));
                    }
                }
                lemma_sum_zero(group.totals@, |total: Total| if total.year == year { if field == 0 { total.count as int } else { total.profit as int } } else { 0 });
                assert(total_at(group.totals@, year, field) == 0);
            }
        }
        let weight = group_weight(name, year, field);
        lemma_sum_ext(groups, weight, |group: Group| 0int);
        lemma_sum_zero(groups, |group: Group| 0int);
        reveal(groups_at);
        assert(groups_at(groups, name, year, field) == sum(groups, weight));
        assert(groups_at(groups, name, year, field) == 0);
        assert(false);
    }
}

pub proof fn lemma_cache_meaning(state: Pages<Vec<Group>>, keys: Seq<i64>)
    requires groups_shape(state), keys.no_duplicates(),
        forall|key: i64| #[trigger] state.at(key).is_some() <==> keys.contains(key),
    ensures cache_rows(state, keys).map_values(|row: OutRow| output_key(row)).no_duplicates(),
        forall|index: int| 0 <= index < cache_rows(state, keys).len() ==> {
            let row = #[trigger] cache_rows(state, keys)[index];
            page_group(state, row.nation, row.o_year, 0) > 0 && row.sum_profit == page_group(state, row.nation, row.o_year, 1)
        },
        forall|name: Seq<u8>, year: int| #[trigger] page_group(state, name, year, 0) > 0 ==>
            exists|row: OutRow| #[trigger] cache_rows(state, keys).contains(row) && row.nation == name && row.o_year == year,
{
    let pages = keys.map_values(|key: i64| cache_page(state, key));
    let key_pages = pages.map_values(|page: Seq<OutRow>| page.map_values(|row: OutRow| output_key(row)));
    verus_prooflib::lemma_map_flat(pages, |row: OutRow| output_key(row));
    assert forall|position: int| 0 <= position < key_pages.len() implies (#[trigger] key_pages[position]).no_duplicates() by {
        let key = keys[position];
        assert(keys.contains(key));
        assert(state.at(key).is_some());
        let groups = state.at(key).unwrap();
        assert(bucket_valid(groups@));
        lemma_bucket_unique(groups@);
    }
    assert forall|left: int, right: int, key: (Seq<u8>, int)| #![trigger key_pages[left].contains(key), key_pages[right].contains(key)]
        0 <= left < right < key_pages.len() implies !(key_pages[left].contains(key) && key_pages[right].contains(key)) by {
        if key_pages[left].contains(key) && key_pages[right].contains(key) {
            let left_index = choose|index: int| 0 <= index < key_pages[left].len() && (#[trigger] key_pages[left][index]) == key;
            let right_index = choose|index: int| 0 <= index < key_pages[right].len() && (#[trigger] key_pages[right][index]) == key;
            let left_row = pages[left][left_index];
            let right_row = pages[right][right_index];
            assert(pages[left].contains(left_row)); assert(pages[right].contains(right_row));
            let left_groups = state.at(keys[left]).unwrap();
            let right_groups = state.at(keys[right]).unwrap();
            lemma_bucket_member(left_groups@, left_row);
            lemma_bucket_member(right_groups@, right_row);
            let left_group = choose|group_index: int, total_index: int| 0 <= group_index < left_groups.len()
                && 0 <= total_index < left_groups[group_index].totals.len() && #[trigger] group_rows(left_groups[group_index])[total_index] == left_row;
            let right_group = choose|group_index: int, total_index: int| 0 <= group_index < right_groups.len()
                && 0 <= total_index < right_groups[group_index].totals.len() && #[trigger] group_rows(right_groups[group_index])[total_index] == right_row;
            assert(name_hash(left_groups[left_group.0].name@) == keys[left] as int);
            assert(name_hash(right_groups[right_group.0].name@) == keys[right] as int);
            assert(keys[left] != keys[right]);
        }
    }
    verus_prooflib::lemma_flat_unique(key_pages);
    assert forall|index: int| 0 <= index < cache_rows(state, keys).len() implies {
        let row = #[trigger] cache_rows(state, keys)[index];
        page_group(state, row.nation, row.o_year, 0) > 0 && row.sum_profit == page_group(state, row.nation, row.o_year, 1)
    } by {
        let row = cache_rows(state, keys)[index];
        assert(flat(pages).contains(row));
        lemma_flat_contains(pages, row);
        let position = choose|position: int| 0 <= position < pages.len() && (#[trigger] pages[position]).contains(row);
        let groups = state.at(keys[position]).unwrap();
        lemma_bucket_member(groups@, row);
        let cell = choose|group_index: int, total_index: int| 0 <= group_index < groups.len()
            && 0 <= total_index < groups[group_index].totals.len() && #[trigger] group_rows(groups[group_index])[total_index] == row;
        let group = groups[cell.0];
        assert(bucket_valid(groups@));
        lemma_bucket_cell(groups@, cell.0, cell.1);
        assert(name_hash(group.name@) == keys[position] as int);
        assert(group.totals[cell.1].count > 0);
    }
    assert forall|name: Seq<u8>, year: int| #[trigger] page_group(state, name, year, 0) > 0 implies
        exists|row: OutRow| #[trigger] cache_rows(state, keys).contains(row) && row.nation == name && row.o_year == year by {
        let key = name_hash(name) as i64;
        let groups = state.at(key).unwrap();
        assert(state.at(key).is_some());
        assert(keys.contains(key));
        let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
        lemma_bucket_support(groups@, name, year, 0);
        let row = choose|row: OutRow| #[trigger] bucket_rows(groups@).contains(row) && row.nation == name && row.o_year == year;
        assert(pages[position].contains(row));
        lemma_flat_contains(pages, row);
    }
}

}
