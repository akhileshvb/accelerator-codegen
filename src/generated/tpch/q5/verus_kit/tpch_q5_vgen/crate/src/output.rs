use vstd::prelude::*;
use std::collections::HashMap;
use crate::storage::*;
use crate::catalog::*;
use verus_common::sums::*;

verus! {

pub struct OutputGroup {
    pub name: Vec<u8>,
    pub count: i128,
    pub revenue: i128,
}

pub open spec fn group_count_weight(row: OutputGroup, name: Seq<u8>) -> int {
    if row.name@ == name { row.count as int } else { 0 }
}

pub open spec fn group_revenue_weight(row: OutputGroup, name: Seq<u8>) -> int {
    if row.name@ == name { row.revenue as int } else { 0 }
}

pub open spec fn group_total(rows: Seq<OutputGroup>, name: Seq<u8>) -> (int, int) {
    (sum(rows, |row: OutputGroup| group_count_weight(row, name)),
     sum(rows, |row: OutputGroup| group_revenue_weight(row, name)))
}

pub open spec fn groups_wf(rows: Seq<OutputGroup>) -> bool {
    &&& forall|position: int| 0 <= position < rows.len() ==> (#[trigger] rows[position]).count > 0
    &&& forall|first: int, second: int| 0 <= first < second < rows.len() ==>
        (#[trigger] rows[first]).name@ != (#[trigger] rows[second]).name@
}

pub proof fn lemma_group_row(rows: Seq<OutputGroup>, position: int)
    requires groups_wf(rows), 0 <= position < rows.len(),
    ensures group_total(rows, rows[position].name@) == (rows[position].count as int, rows[position].revenue as int),
{
    let name = rows[position].name@;
    assert forall|other: int| 0 <= other < rows.len() && other != position implies (#[trigger] rows[other]).name@ != name by {
        if other < position { assert(rows[other].name@ != rows[position].name@); }
        else { assert(rows[position].name@ != rows[other].name@); }
    }
    lemma_sum_single_match(rows, |row: OutputGroup| group_count_weight(row, name), position);
    lemma_sum_single_match(rows, |row: OutputGroup| group_revenue_weight(row, name), position);
    reveal(group_total);
    assert(group_total(rows, name).0 == sum(rows, |row: OutputGroup| group_count_weight(row, name)));
    assert(group_total(rows, name).1 == sum(rows, |row: OutputGroup| group_revenue_weight(row, name)));
    assert(group_total(rows, name).0 == rows[position].count);
    assert(group_total(rows, name).1 == rows[position].revenue);
}

pub fn add_group(rows: &mut Vec<OutputGroup>, name: &[u8], count: i128, revenue: i128) -> (ok: bool)
    requires groups_wf(old(rows)@), count > 0,
    ensures ok ==> groups_wf(final(rows)@),
        ok ==> forall|text: Seq<u8>| #[trigger] group_total(final(rows)@, text) ==
            (group_total(old(rows)@, text).0 + if text == name@ { count as int } else { 0 },
             group_total(old(rows)@, text).1 + if text == name@ { revenue as int } else { 0 }),
{
    let ghost entry = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == entry, entry == old(rows)@, groups_wf(entry), count > 0,
            forall|position: int| 0 <= position < index ==> (#[trigger] entry[position]).name@ != name@,
        decreases rows.len() - index,
    {
        if bytes_equal(&rows[index].name, name) {
            let next_count = match rows[index].count.checked_add(count) { Some(value) => value, None => return false };
            let next_revenue = match rows[index].revenue.checked_add(revenue) { Some(value) => value, None => return false };
            let replacement = OutputGroup { name: bytes_copy(name), count: next_count, revenue: next_revenue };
            proof {
                assert(entry[index as int].count > 0);
                assert(next_count > 0);
                assert forall|text: Seq<u8>| #[trigger] group_total(entry.update(index as int, replacement), text) ==
                    (group_total(entry, text).0 + if text == name@ { count as int } else { 0 },
                     group_total(entry, text).1 + if text == name@ { revenue as int } else { 0 }) by {
                    lemma_sum_update(entry, index as int, replacement, |row: OutputGroup| group_count_weight(row, text));
                    lemma_sum_update(entry, index as int, replacement, |row: OutputGroup| group_revenue_weight(row, text));
                }
            }
            rows.set(index, replacement);
            proof {
                assert forall|position: int| 0 <= position < rows@.len() implies (#[trigger] rows@[position]).count > 0 by {
                    if position != index { assert(rows@[position].count == entry[position].count); }
                }
                assert forall|first: int, second: int| 0 <= first < second < rows@.len() implies
                    (#[trigger] rows@[first]).name@ != (#[trigger] rows@[second]).name@ by {
                    assert(rows@[first].name@ == entry[first].name@);
                    assert(rows@[second].name@ == entry[second].name@);
                    assert(entry[first].name@ != entry[second].name@);
                }
            }
            return true;
        }
        index += 1;
    }
    let addition = OutputGroup { name: bytes_copy(name), count, revenue };
    proof {
        assert forall|text: Seq<u8>| #[trigger] group_total(entry.push(addition), text) ==
            (group_total(entry, text).0 + if text == name@ { count as int } else { 0 },
             group_total(entry, text).1 + if text == name@ { revenue as int } else { 0 }) by {
            lemma_sum_push(entry, addition, |row: OutputGroup| group_count_weight(row, text));
            lemma_sum_push(entry, addition, |row: OutputGroup| group_revenue_weight(row, text));
        }
    }
    rows.push(addition);
    true
}

pub open spec fn window_cells(rows: Seq<Cell>, start: i32, end: i32) -> (int, int) {
    (sum(rows, |cell: Cell| if start <= cell.day < end { cell.count as int } else { 0 }),
     sum(rows, |cell: Cell| if start <= cell.day < end { cell.revenue as int } else { 0 }))
}

pub open spec fn window_value(profiles: Table, nation: i64, start: i32, end: i32) -> (int, int) {
    window_cells(profiles.rows(nation), start, end)
}

pub proof fn lemma_window_empty(rows: Seq<Cell>, start: i32, end: i32)
    requires start >= end,
    ensures window_cells(rows, start, end) == (0, 0),
{
    lemma_sum_zero(rows, |cell: Cell| if start <= cell.day < end { cell.count as int } else { 0 });
    lemma_sum_zero(rows, |cell: Cell| if start <= cell.day < end { cell.revenue as int } else { 0 });
}

pub proof fn lemma_window_step(group: Group, start: i32, day: i32)
    requires group.wf(), start <= day, day < i32::MAX,
        forall|index: int| 0 <= index < group.rows@.len() ==> (#[trigger] group.rows@[index]).sub == 0,
    ensures window_cells(group.rows@, start, (day + 1) as i32) ==
        (window_cells(group.rows@, start, day).0 + group.at(0, day).0,
         window_cells(group.rows@, start, day).1 + group.at(0, day).1),
{
    let count_before = |cell: Cell| if start <= cell.day < day { cell.count as int } else { 0 };
    let count_day = |cell: Cell| if cell.day == day { cell.count as int } else { 0 };
    let count_after = |cell: Cell| if start <= cell.day < day + 1 { cell.count as int } else { 0 };
    let revenue_before = |cell: Cell| if start <= cell.day < day { cell.revenue as int } else { 0 };
    let revenue_day = |cell: Cell| if cell.day == day { cell.revenue as int } else { 0 };
    let revenue_after = |cell: Cell| if start <= cell.day < day + 1 { cell.revenue as int } else { 0 };
    assert forall|cell: Cell| #[trigger] count_after(cell) == count_before(cell) + count_day(cell) by {}
    assert forall|cell: Cell| #[trigger] revenue_after(cell) == revenue_before(cell) + revenue_day(cell) by {}
    lemma_sum_add(group.rows@, count_before, count_day, count_after);
    lemma_sum_add(group.rows@, revenue_before, revenue_day, revenue_after);
    lemma_sum_ext(group.rows@, count_day, |cell: Cell| if cell.sub == 0 && cell.day == day { cell.count as int } else { 0 });
    lemma_sum_ext(group.rows@, revenue_day, |cell: Cell| if cell.sub == 0 && cell.day == day { cell.revenue as int } else { 0 });
    group.lemma_at(0, day);
}

pub open spec fn region_count_weight(region: Binding, key: i64, name: Seq<u8>) -> int {
    if region.key == key && region.name@ == name { region.count as int } else { 0 }
}

pub open spec fn catalog_weight(regions: Seq<Binding>, nation: Binding, name: Seq<u8>) -> int {
    sum(regions, |region: Binding| region_count_weight(region, nation.region, name))
}

pub open spec fn catalog_nation_value(profiles: Table, nation: Binding, regions: Seq<Binding>,
    name: Seq<u8>, start: i32, end: i32, group: Seq<u8>) -> (int, int) {
    if nation.name@ == group {
        (nation.count as int * catalog_weight(regions, nation, name) * window_value(profiles, nation.key, start, end).0,
         nation.count as int * catalog_weight(regions, nation, name) * window_value(profiles, nation.key, start, end).1)
    } else { (0, 0) }
}

pub open spec fn catalog_value(profiles: Table, nations: Seq<Binding>, regions: Seq<Binding>,
    name: Seq<u8>, start: i32, end: i32, group: Seq<u8>) -> (int, int) {
    (sum(nations, |nation: Binding| catalog_nation_value(profiles, nation, regions, name, start, end, group).0),
     sum(nations, |nation: Binding| catalog_nation_value(profiles, nation, regions, name, start, end, group).1))
}

pub proof fn lemma_window_shape(profiles: Table, nation: i64, start: i32, end: i32)
    requires profiles.wf(), profiles.daily(),
    ensures window_value(profiles, nation, start, end).0 >= 0,
        window_value(profiles, nation, start, end).0 == 0 ==> window_value(profiles, nation, start, end).1 == 0,
{
    let rows = profiles.rows(nation);
    profiles.lemma_rows(nation, 0, 0);
    let count = |cell: Cell| if start <= cell.day < end { cell.count as int } else { 0 };
    let revenue = |cell: Cell| if start <= cell.day < end { cell.revenue as int } else { 0 };
    assert forall|position: int| 0 <= position < rows.len() implies count(#[trigger] rows[position]) >= 0 by {
        assert(profiles.at(nation, rows[position].sub, rows[position].day).0 >= 0);
    }
    lemma_sum_nonneg(rows, count);
    if window_value(profiles, nation, start, end).0 == 0 {
        assert forall|position: int| 0 <= position < rows.len() implies revenue(#[trigger] rows[position]) == 0 by {
            lemma_sum_ge_term(rows, count, position);
            if start <= rows[position].day < end {
                assert(rows[position].count == 0);
                assert(profiles.at(nation, rows[position].sub, rows[position].day).0 == 0);
                assert(profiles.at(nation, rows[position].sub, rows[position].day).1 == 0);
            }
        }
        lemma_sum_zero(rows, revenue);
    }
}

pub open spec fn nation_value(profiles: Table, nation: Binding, region: Binding,
    name: Seq<u8>, start: i32, end: i32, group: Seq<u8>) -> (int, int) {
    if region.name@ == name && nation.region == region.key && nation.name@ == group {
        let window = window_value(profiles, nation.key, start, end);
        (window.0 * (nation.count as int * region.count), window.1 * (nation.count as int * region.count))
    } else { (0, 0) }
}

pub open spec fn region_value(profiles: Table, nations: Seq<Binding>, region: Binding,
    name: Seq<u8>, start: i32, end: i32, group: Seq<u8>) -> (int, int) {
    (sum(nations, |nation: Binding| nation_value(profiles, nation, region, name, start, end, group).0),
     sum(nations, |nation: Binding| nation_value(profiles, nation, region, name, start, end, group).1))
}

pub proof fn lemma_catalog_transpose(profiles: Table, nations: Seq<Binding>, regions: Seq<Binding>,
    name: Seq<u8>, start: i32, end: i32, group: Seq<u8>)
    ensures catalog_value(profiles, nations, regions, name, start, end, group) ==
        (sum(regions, |region: Binding| region_value(profiles, nations, region, name, start, end, group).0),
         sum(regions, |region: Binding| region_value(profiles, nations, region, name, start, end, group).1)),
{
    let count_by_nation = |nation: Binding| sum(regions, |region: Binding| nation_value(profiles, nation, region, name, start, end, group).0);
    let revenue_by_nation = |nation: Binding| sum(regions, |region: Binding| nation_value(profiles, nation, region, name, start, end, group).1);
    assert forall|nation: Binding| #[trigger] count_by_nation(nation) ==
        if nation.name@ == group { nation.count as int * catalog_weight(regions, nation, name) * window_value(profiles, nation.key, start, end).0 } else { 0 } by {
        let raw = |region: Binding| region_count_weight(region, nation.region, name);
        let scaled = |region: Binding| nation_value(profiles, nation, region, name, start, end, group).0;
        let factor = if nation.name@ == group { nation.count as int * window_value(profiles, nation.key, start, end).0 } else { 0 };
        assert forall|region: Binding| #[trigger] scaled(region) == factor * raw(region) by {
            assert(window_value(profiles, nation.key, start, end).0 * (nation.count as int * region.count) ==
                (nation.count as int * window_value(profiles, nation.key, start, end).0) * region.count) by (nonlinear_arith);
        }
        lemma_sum_scale(regions, raw, scaled, factor);
        lemma_sum_ext_all(regions, raw, |region: Binding| region_count_weight(region, nation.region, name));
        assert(sum(regions, raw) == catalog_weight(regions, nation, name));
        assert(count_by_nation(nation) == factor * catalog_weight(regions, nation, name));
        assert((nation.count as int * window_value(profiles, nation.key, start, end).0) * catalog_weight(regions, nation, name) ==
            nation.count as int * catalog_weight(regions, nation, name) * window_value(profiles, nation.key, start, end).0) by (nonlinear_arith);
    }
    assert forall|nation: Binding| #[trigger] revenue_by_nation(nation) ==
        if nation.name@ == group { nation.count as int * catalog_weight(regions, nation, name) * window_value(profiles, nation.key, start, end).1 } else { 0 } by {
        let raw = |region: Binding| region_count_weight(region, nation.region, name);
        let scaled = |region: Binding| nation_value(profiles, nation, region, name, start, end, group).1;
        let factor = if nation.name@ == group { nation.count as int * window_value(profiles, nation.key, start, end).1 } else { 0 };
        assert forall|region: Binding| #[trigger] scaled(region) == factor * raw(region) by {
            assert(window_value(profiles, nation.key, start, end).1 * (nation.count as int * region.count) ==
                (nation.count as int * window_value(profiles, nation.key, start, end).1) * region.count) by (nonlinear_arith);
        }
        lemma_sum_scale(regions, raw, scaled, factor);
        lemma_sum_ext_all(regions, raw, |region: Binding| region_count_weight(region, nation.region, name));
        assert(sum(regions, raw) == catalog_weight(regions, nation, name));
        assert(revenue_by_nation(nation) == factor * catalog_weight(regions, nation, name));
        assert((nation.count as int * window_value(profiles, nation.key, start, end).1) * catalog_weight(regions, nation, name) ==
            nation.count as int * catalog_weight(regions, nation, name) * window_value(profiles, nation.key, start, end).1) by (nonlinear_arith);
    }
    let joint_count = |nation: Binding, region: Binding| nation_value(profiles, nation, region, name, start, end, group).0;
    let joint_revenue = |nation: Binding, region: Binding| nation_value(profiles, nation, region, name, start, end, group).1;
    let catalog_count = |nation: Binding| catalog_nation_value(profiles, nation, regions, name, start, end, group).0;
    let catalog_revenue = |nation: Binding| catalog_nation_value(profiles, nation, regions, name, start, end, group).1;
    let region_count = |region: Binding| region_value(profiles, nations, region, name, start, end, group).0;
    let region_revenue = |region: Binding| region_value(profiles, nations, region, name, start, end, group).1;
    assert forall|nation: Binding| #[trigger] catalog_count(nation) == sum(regions, |region: Binding| joint_count(nation, region)) by {
        assert(catalog_count(nation) == count_by_nation(nation));
        lemma_sum_ext_all(regions, |region: Binding| nation_value(profiles, nation, region, name, start, end, group).0, |region: Binding| joint_count(nation, region));
    }
    assert forall|nation: Binding| #[trigger] catalog_revenue(nation) == sum(regions, |region: Binding| joint_revenue(nation, region)) by {
        assert(catalog_revenue(nation) == revenue_by_nation(nation));
        lemma_sum_ext_all(regions, |region: Binding| nation_value(profiles, nation, region, name, start, end, group).1, |region: Binding| joint_revenue(nation, region));
    }
    assert forall|region: Binding| #[trigger] region_count(region) == sum(nations, |nation: Binding| joint_count(nation, region)) by {
        lemma_sum_ext_all(nations, |nation: Binding| nation_value(profiles, nation, region, name, start, end, group).0, |nation: Binding| joint_count(nation, region));
    }
    assert forall|region: Binding| #[trigger] region_revenue(region) == sum(nations, |nation: Binding| joint_revenue(nation, region)) by {
        lemma_sum_ext_all(nations, |nation: Binding| nation_value(profiles, nation, region, name, start, end, group).1, |nation: Binding| joint_revenue(nation, region));
    }
    lemma_sum_swap(nations, regions, joint_count, catalog_count, region_count);
    lemma_sum_swap(nations, regions, joint_revenue, catalog_revenue, region_revenue);
}

pub proof fn lemma_catalog_empty(profiles: Table, nations: Seq<Binding>, regions: Seq<Binding>,
    name: Seq<u8>, start: i32, end: i32)
    requires start >= end || (forall|position: int| 0 <= position < regions.len() ==> (#[trigger] regions[position]).name@ != name),
    ensures forall|group: Seq<u8>| #[trigger] catalog_value(profiles, nations, regions, name, start, end, group) == (0, 0),
{
    assert forall|group: Seq<u8>| #[trigger] catalog_value(profiles, nations, regions, name, start, end, group) == (0, 0) by {
        let count = |nation: Binding| catalog_nation_value(profiles, nation, regions, name, start, end, group).0;
        let revenue = |nation: Binding| catalog_nation_value(profiles, nation, regions, name, start, end, group).1;
        assert forall|position: int| 0 <= position < nations.len() implies count(#[trigger] nations[position]) == 0 && revenue(nations[position]) == 0 by {
            let nation = nations[position];
            if start >= end { lemma_window_empty(profiles.rows(nation.key), start, end); }
            else { lemma_sum_zero(regions, |region: Binding| region_count_weight(region, nation.region, name)); }
            if start >= end {
                assert(window_value(profiles, nation.key, start, end) == (0, 0));
            } else {
                assert(catalog_weight(regions, nation, name) == 0);
                assert(nation.count as int * 0int * window_value(profiles, nation.key, start, end).0 == 0) by (nonlinear_arith);
                assert(nation.count as int * 0int * window_value(profiles, nation.key, start, end).1 == 0) by (nonlinear_arith);
            }
        }
        lemma_sum_zero(nations, count);
        lemma_sum_zero(nations, revenue);
    }
}

pub fn window(profiles: &Table, nation: i64, start: i32, end: i32) -> (out: Option<Cell>)
    requires profiles.wf(), profiles.daily(),
    ensures out matches Some(cell) ==> (cell.count as int, cell.revenue as int) == window_value(*profiles, nation, start, end),
{
    let mut count = 0i128;
    let mut revenue = 0i128;
    if start >= end {
        proof { lemma_window_empty(profiles.rows(nation), start, end); }
        return Some(Cell { sub: nation, day: 0, count, revenue });
    }
    let group = match profiles.group(nation) { Some(group) => group, None => return Some(Cell { sub: nation, day: 0, count, revenue }) };
    let mut index = 0;
    while index < group.rows.len()
        invariant group.wf(), group.rows@ == profiles.rows(nation), index <= group.rows.len(),
            count == sum(group.rows@.take(index as int), |cell: Cell| if start <= cell.day < end { cell.count as int } else { 0 }),
            revenue == sum(group.rows@.take(index as int), |cell: Cell| if start <= cell.day < end { cell.revenue as int } else { 0 }),
        decreases group.rows.len() - index,
    {
        let cell = group.rows[index];
        proof {
            lemma_sum_take(group.rows@, index as int, |cell: Cell| if start <= cell.day < end { cell.count as int } else { 0 });
            lemma_sum_take(group.rows@, index as int, |cell: Cell| if start <= cell.day < end { cell.revenue as int } else { 0 });
        }
        if start <= cell.day && cell.day < end {
            count = match count.checked_add(cell.count) { Some(value) => value, None => return None };
            revenue = match revenue.checked_add(cell.revenue) { Some(value) => value, None => return None };
        }
        index += 1;
    }
    proof { assert(group.rows@.take(group.rows@.len() as int) =~= group.rows@); }
    Some(Cell { sub: nation, day: 0, count, revenue })
}

pub open spec fn output_name_count(rows: Seq<(Vec<u8>, i128)>, name: Seq<u8>) -> int {
    sum(rows, |row: (Vec<u8>, i128)| output_name_weight(row, name))
}

pub open spec fn output_name_weight(row: (Vec<u8>, i128), name: Seq<u8>) -> int {
    ind(row.0@ == name)
}

pub open spec fn source_name_weight(row: OutputGroup, name: Seq<u8>) -> int {
    ind(row.name@ == name)
}

pub fn insertion_position(rows: &Vec<(Vec<u8>, i128)>, revenue: i128) -> (position: usize)
    ensures position <= rows@.len(),
        forall|before: int| 0 <= before < position ==> (#[trigger] rows@[before]).1 >= revenue,
        position < rows@.len() ==> rows@[position as int].1 < revenue,
{
    let mut position = 0;
    while position < rows.len()
        invariant position <= rows@.len(),
            forall|before: int| 0 <= before < position ==> (#[trigger] rows@[before]).1 >= revenue,
        decreases rows.len() - position,
    {
        if rows[position].1 < revenue { return position; }
        position += 1;
    }
    position
}

pub fn finish(groups: &Vec<OutputGroup>) -> (out: Vec<(Vec<u8>, i128)>)
    requires groups_wf(groups@),
    ensures forall|position: int| 0 <= position < out@.len() ==>
        (#[trigger] out@[position]).1 == group_total(groups@, out@[position].0@).1 && group_total(groups@, out@[position].0@).0 > 0,
        forall|first: int, second: int| 0 <= first < second < out@.len() ==>
            (#[trigger] out@[first]).0@ != (#[trigger] out@[second]).0@ && out@[first].1 >= out@[second].1,
        forall|name: Seq<u8>| (#[trigger] group_total(groups@, name)).0 > 0 ==>
            exists|position: int| 0 <= position < out@.len() && (#[trigger] out@[position]).0@ == name,
{
    let mut out: Vec<(Vec<u8>, i128)> = Vec::new();
    let mut index = 0;
    while index < groups.len()
        invariant index <= groups.len(), out@.len() == index, groups_wf(groups@),
            forall|position: int| 0 <= position < out@.len() ==>
                (#[trigger] out@[position]).1 == group_total(groups@, out@[position].0@).1 && group_total(groups@, out@[position].0@).0 > 0,
            forall|first: int, second: int| 0 <= first < second < out@.len() ==>
                (#[trigger] out@[first]).0@ != (#[trigger] out@[second]).0@ && out@[first].1 >= out@[second].1,
            forall|name: Seq<u8>| #[trigger] output_name_count(out@, name) ==
                sum(groups@.take(index as int), |row: OutputGroup| source_name_weight(row, name)),
        decreases groups.len() - index,
    {
        let group = &groups[index];
        let ghost previous = out@;
        proof {
            lemma_group_row(groups@, index as int);
            assert forall|position: int| 0 <= position < previous.len() implies (#[trigger] previous[position]).0@ != group.name@ by {
                let weight = |row: OutputGroup| source_name_weight(row, group.name@);
                assert forall|source: int| 0 <= source < index implies (#[trigger] groups@[source]).name@ != group.name@ by {
                    assert(groups@[source].name@ != groups@[index as int].name@);
                }
                lemma_sum_zero(groups@.take(index as int), weight);
                assert(output_name_count(previous, group.name@) == 0);
                if previous[position].0@ == group.name@ {
                    lemma_sum_pos(previous, |row: (Vec<u8>, i128)| output_name_weight(row, group.name@), position);
                }
            }
        }
        let position = insertion_position(&out, group.revenue);
        let row = (bytes_copy(&group.name), group.revenue);
        proof {
            assert forall|after: int| position <= after < previous.len() implies row.1 >= (#[trigger] previous[after]).1 by {
                if position < previous.len() { assert(previous[position as int].1 < row.1); }
            }
            verus_prooflib::lemma_sorted_insert_at(previous, position as int, row, |left: (Vec<u8>, i128), right: (Vec<u8>, i128)| left.1 >= right.1);
            assert forall|name: Seq<u8>| #[trigger] output_name_count(previous.insert(position as int, row), name) ==
                sum(groups@.take(index as int + 1), |group: OutputGroup| source_name_weight(group, name)) by {
                verus_prooflib::lemma_sum_insert(previous, position as int, row, |entry: (Vec<u8>, i128)| output_name_weight(entry, name));
                lemma_sum_take(groups@, index as int, |group: OutputGroup| source_name_weight(group, name));
                assert(output_name_count(previous, name) == sum(groups@.take(index as int), |group: OutputGroup| source_name_weight(group, name)));
                assert(output_name_weight(row, name) == source_name_weight(groups@[index as int], name));
                assert(output_name_count(previous.insert(position as int, row), name) == output_name_count(previous, name) + output_name_weight(row, name));
            }
            assert forall|first: int, second: int| 0 <= first < second < previous.insert(position as int, row).len() implies
                (#[trigger] previous.insert(position as int, row)[first]).0@ != (#[trigger] previous.insert(position as int, row)[second]).0@ by {
                let source_first = if first < position { first } else { first - 1 };
                let source_second = if second < position { second } else { second - 1 };
                if first == position { assert(row.0@ != previous[source_second].0@); }
                else if second == position { assert(previous[source_first].0@ != row.0@); }
                else { assert(previous[source_first].0@ != previous[source_second].0@); }
            }
        }
        out.insert(position, row);
        index += 1;
    }
    proof {
        assert(groups@.take(groups@.len() as int) =~= groups@);
        assert forall|name: Seq<u8>| (#[trigger] group_total(groups@, name)).0 > 0 implies
            exists|position: int| 0 <= position < out@.len() && (#[trigger] out@[position]).0@ == name by {
            let source = lemma_sum_nonzero_witness(groups@, |row: OutputGroup| group_count_weight(row, name));
            lemma_sum_pos(groups@, |row: OutputGroup| source_name_weight(row, name), source);
            assert(output_name_count(out@, name) > 0);
            let position = lemma_sum_nonzero_witness(out@, |row: (Vec<u8>, i128)| output_name_weight(row, name));
            assert(out@[position].0@ == name);
        }
    }
    out
}

pub fn run(profiles: &Table, nations: &Vec<Binding>, regions: &Vec<Binding>,
    nation_regions: &HashMap<i64, Vec<usize>>, region_names: &HashMap<u64, Vec<usize>>,
    name: &[u8], start: i32, end: i32) -> (out: Option<Vec<(Vec<u8>, i128)>>)
    requires profiles.wf(), profiles.daily(), regions_indexed(nations@, nation_regions@), names_indexed(regions@, region_names@),
        forall|index: int| 0 <= index < nations@.len() ==> (#[trigger] nations@[index]).count > 0,
        forall|index: int| 0 <= index < regions@.len() ==> (#[trigger] regions@[index]).count > 0,
    ensures out matches Some(rows) ==>
        (forall|index: int| 0 <= index < rows@.len() ==>
            catalog_value(*profiles, nations@, regions@, name@, start, end, (#[trigger] rows@[index]).0@).0 > 0
            && rows@[index].1 == catalog_value(*profiles, nations@, regions@, name@, start, end, rows@[index].0@).1)
        && (forall|first: int, second: int| 0 <= first < second < rows@.len() ==>
            (#[trigger] rows@[first]).0@ != (#[trigger] rows@[second]).0@
            && rows@[first].1 >= rows@[second].1)
        && (forall|group: Seq<u8>| (#[trigger] catalog_value(*profiles, nations@, regions@, name@, start, end, group)).0 > 0 ==>
            exists|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0@ == group),
{
    let mut groups: Vec<OutputGroup> = Vec::new();
    let mut windows: HashMap<i64, Cell> = HashMap::new();
    let nation_len = nations.len();
    let region_len = regions.len();
    if start >= end {
        proof { lemma_catalog_empty(*profiles, nations@, regions@, name@, start, end); }
        return Some(Vec::new());
    }
    let name_key = hash_name(name);
    let selected_regions = match region_names.get(&name_key) {
        Some(rows) => rows,
        None => {
            proof {
                assert forall|position: int| 0 <= position < regions@.len() implies (#[trigger] regions@[position]).name@ != name@ by {
                    if regions@[position].name@ == name@ {
                        assert(name_hash(regions@[position].name@) == name_key);
                        assert(region_names@.contains_key(name_key));
                    }
                }
                lemma_catalog_empty(*profiles, nations@, regions@, name@, start, end);
            }
            return Some(Vec::new());
        },
    };
    let mut region_index = 0;
    while region_index < selected_regions.len()
        invariant region_index <= selected_regions.len(), profiles.wf(), profiles.daily(), groups_wf(groups@),
            nations@.len() == nation_len, regions@.len() == region_len,
            regions_indexed(nations@, nation_regions@), names_indexed(regions@, region_names@),
            region_names@.contains_key(name_key), selected_regions@ == (region_names@[name_key])@,
            name_key == name_hash(name@),
            forall|position: int| 0 <= position < nations@.len() ==> (#[trigger] nations@[position]).count > 0,
            forall|position: int| 0 <= position < regions@.len() ==> (#[trigger] regions@[position]).count > 0,
            forall|key: i64| windows@.contains_key(key) ==> ((#[trigger] windows@[key]).count as int, windows@[key].revenue as int) == window_value(*profiles, key, start, end),
            forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                (sum(selected_regions@.take(region_index as int), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0),
                 sum(selected_regions@.take(region_index as int), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1)),
        decreases selected_regions.len() - region_index,
    {
        proof {
            assert(selected_regions@.contains(selected_regions@[region_index as int]));
            assert(selected_regions@[region_index as int] < regions@.len());
        }
        let region = &regions[selected_regions[region_index]];
        let ghost before_region = groups@;
        if bytes_equal(&region.name, name) {
            let selected_nations = match nation_regions.get(&region.key) {
                Some(rows) => rows,
                None => {
                    proof {
                        assert forall|group: Seq<u8>| #[trigger] region_value(*profiles, nations@, *region, name@, start, end, group) == (0, 0) by {
                            let count = |nation: Binding| nation_value(*profiles, nation, *region, name@, start, end, group).0;
                            let revenue = |nation: Binding| nation_value(*profiles, nation, *region, name@, start, end, group).1;
                            assert forall|position: int| 0 <= position < nations@.len() implies (#[trigger] nations@[position]).region != region.key by {
                                if nations@[position].region == region.key { assert(nation_regions@.contains_key(region.key)); }
                            }
                            lemma_sum_zero(nations@, count);
                            lemma_sum_zero(nations@, revenue);
                        }
                        assert forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                            (sum(selected_regions@.take(region_index as int + 1), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0),
                             sum(selected_regions@.take(region_index as int + 1), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1)) by {
                            lemma_sum_take(selected_regions@, region_index as int, |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0);
                            lemma_sum_take(selected_regions@, region_index as int, |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1);
                        }
                    }
                    region_index += 1;
                    continue;
                },
            };
            let mut nation_index = 0;
            while nation_index < selected_nations.len()
                invariant nation_index <= selected_nations.len(), profiles.wf(), profiles.daily(), groups_wf(groups@),
                    nations@.len() == nation_len, regions@.len() == region_len,
                    region_index < selected_regions.len(), selected_regions@[region_index as int] < regions@.len(),
                    region == regions@[selected_regions@[region_index as int] as int],
                    region.count > 0, region.name@ == name@,
                    regions_indexed(nations@, nation_regions@), names_indexed(regions@, region_names@),
                    region_names@.contains_key(name_key), selected_regions@ == (region_names@[name_key])@,
                    nation_regions@.contains_key(region.key), selected_nations@ == (nation_regions@[region.key])@,
                    name_key == name_hash(name@),
                    forall|position: int| 0 <= position < nations@.len() ==> (#[trigger] nations@[position]).count > 0,
                    forall|position: int| 0 <= position < regions@.len() ==> (#[trigger] regions@[position]).count > 0,
                    forall|key: i64| windows@.contains_key(key) ==> ((#[trigger] windows@[key]).count as int, windows@[key].revenue as int) == window_value(*profiles, key, start, end),
                    forall|group: Seq<u8>| #[trigger] group_total(before_region, group) ==
                        (sum(selected_regions@.take(region_index as int), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0),
                         sum(selected_regions@.take(region_index as int), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1)),
                    forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                        (group_total(before_region, group).0 + sum(selected_nations@.take(nation_index as int), |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).0),
                         group_total(before_region, group).1 + sum(selected_nations@.take(nation_index as int), |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).1)),
                decreases selected_nations.len() - nation_index,
            {
                proof {
                    assert(selected_nations@.contains(selected_nations@[nation_index as int]));
                    assert(selected_nations@[nation_index as int] < nations@.len());
                }
                let nation = &nations[selected_nations[nation_index]];
                let ghost before_nation = groups@;
                if nation.region == region.key {
                    let cell = match windows.get(&nation.key) {
                        Some(cell) => *cell,
                        None => {
                            let cell = match window(profiles, nation.key, start, end) { Some(cell) => cell, None => return None };
                            windows.insert(nation.key, cell);
                            cell
                        },
                    };
                    proof { lemma_window_shape(*profiles, nation.key, start, end); }
                    if cell.count > 0 {
                        let weight = match nation.count.checked_mul(region.count) { Some(value) => value, None => return None };
                        let count = match cell.count.checked_mul(weight) { Some(value) => value, None => return None };
                        let revenue = match cell.revenue.checked_mul(weight) { Some(value) => value, None => return None };
                        proof {
                            verus_prooflib::lemma_positive_product(nation.count as int, region.count as int);
                            verus_prooflib::lemma_positive_product(cell.count as int, weight as int);
                            assert(count > 0);
                        }
                        if !add_group(&mut groups, &nation.name, count, revenue) { return None; }
                    }
                }
                proof {
                    assert forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                        (group_total(before_nation, group).0 + nation_value(*profiles, *nation, *region, name@, start, end, group).0,
                         group_total(before_nation, group).1 + nation_value(*profiles, *nation, *region, name@, start, end, group).1) by {
                        lemma_window_shape(*profiles, nation.key, start, end);
                    }
                    assert forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                        (group_total(before_region, group).0 + sum(selected_nations@.take(nation_index as int + 1), |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).0),
                         group_total(before_region, group).1 + sum(selected_nations@.take(nation_index as int + 1), |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).1)) by {
                        lemma_sum_take(selected_nations@, nation_index as int, |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).0);
                        lemma_sum_take(selected_nations@, nation_index as int, |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).1);
                    }
                }
                nation_index += 1;
            }
            proof {
                assert(selected_nations@.take(selected_nations@.len() as int) =~= selected_nations@);
                assert(selected_nations@.no_duplicates());
                assert forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                    (group_total(before_region, group).0 + region_value(*profiles, nations@, *region, name@, start, end, group).0,
                     group_total(before_region, group).1 + region_value(*profiles, nations@, *region, name@, start, end, group).1) by {
                    let count = |nation: Binding| nation_value(*profiles, nation, *region, name@, start, end, group).0;
                    let revenue = |nation: Binding| nation_value(*profiles, nation, *region, name@, start, end, group).1;
                    let keep = |nation: Binding| nation.region == region.key;
                    verus_prooflib::lemma_sum_selected(nations@, selected_nations@, keep, count);
                    verus_prooflib::lemma_sum_selected(nations@, selected_nations@, keep, revenue);
                    lemma_sum_ext_all(selected_nations@,
                        |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).0,
                        |position: usize| count(nations@[position as int]));
                    lemma_sum_ext_all(selected_nations@,
                        |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).1,
                        |position: usize| revenue(nations@[position as int]));
                    lemma_sum_ext_all(nations@, count, |nation: Binding| if keep(nation) { count(nation) } else { 0 });
                    lemma_sum_ext_all(nations@, revenue, |nation: Binding| if keep(nation) { revenue(nation) } else { 0 });
                    assert(region_value(*profiles, nations@, *region, name@, start, end, group).0 == sum(nations@, count));
                    assert(region_value(*profiles, nations@, *region, name@, start, end, group).1 == sum(nations@, revenue));
                    assert(sum(selected_nations@, |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).0) == sum(nations@, count));
                    assert(sum(selected_nations@, |position: usize| nation_value(*profiles, nations@[position as int], *region, name@, start, end, group).1) == sum(nations@, revenue));
                }
            }
        } else {
            proof {
                assert forall|group: Seq<u8>| #[trigger] region_value(*profiles, nations@, *region, name@, start, end, group) == (0, 0) by {
                    lemma_sum_zero(nations@, |nation: Binding| nation_value(*profiles, nation, *region, name@, start, end, group).0);
                    lemma_sum_zero(nations@, |nation: Binding| nation_value(*profiles, nation, *region, name@, start, end, group).1);
                }
            }
        }
        proof {
            assert forall|group: Seq<u8>| #[trigger] group_total(groups@, group) ==
                (sum(selected_regions@.take(region_index as int + 1), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0),
                 sum(selected_regions@.take(region_index as int + 1), |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1)) by {
                lemma_sum_take(selected_regions@, region_index as int, |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0);
                lemma_sum_take(selected_regions@, region_index as int, |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1);
            }
        }
        region_index += 1;
    }
    proof {
        assert(selected_regions@.take(selected_regions@.len() as int) =~= selected_regions@);
        assert(selected_regions@.no_duplicates());
        assert forall|group: Seq<u8>| #[trigger] group_total(groups@, group) == catalog_value(*profiles, nations@, regions@, name@, start, end, group) by {
            let count = |region: Binding| region_value(*profiles, nations@, region, name@, start, end, group).0;
            let revenue = |region: Binding| region_value(*profiles, nations@, region, name@, start, end, group).1;
            let keep = |region: Binding| name_hash(region.name@) == name_key;
            verus_prooflib::lemma_sum_selected(regions@, selected_regions@, keep, count);
            verus_prooflib::lemma_sum_selected(regions@, selected_regions@, keep, revenue);
            lemma_sum_ext_all(selected_regions@,
                |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).0,
                |position: usize| count(regions@[position as int]));
            lemma_sum_ext_all(selected_regions@,
                |position: usize| region_value(*profiles, nations@, regions@[position as int], name@, start, end, group).1,
                |position: usize| revenue(regions@[position as int]));
            assert forall|region: Binding| #[trigger] count(region) == if keep(region) { count(region) } else { 0 } by {
                if !keep(region) {
                    assert(region.name@ != name@);
                    lemma_sum_zero(nations@, |nation: Binding| nation_value(*profiles, nation, region, name@, start, end, group).0);
                }
            }
            assert forall|region: Binding| #[trigger] revenue(region) == if keep(region) { revenue(region) } else { 0 } by {
                if !keep(region) {
                    assert(region.name@ != name@);
                    lemma_sum_zero(nations@, |nation: Binding| nation_value(*profiles, nation, region, name@, start, end, group).1);
                }
            }
            lemma_sum_ext_all(regions@, count, |region: Binding| if keep(region) { count(region) } else { 0 });
            lemma_sum_ext_all(regions@, revenue, |region: Binding| if keep(region) { revenue(region) } else { 0 });
            lemma_catalog_transpose(*profiles, nations@, regions@, name@, start, end, group);
        }
        assert forall|group: Seq<u8>| #[trigger] catalog_value(*profiles, nations@, regions@, name@, start, end, group) == group_total(groups@, group) by {}
    }
    let out = finish(&groups);
    Some(out)
}

}
