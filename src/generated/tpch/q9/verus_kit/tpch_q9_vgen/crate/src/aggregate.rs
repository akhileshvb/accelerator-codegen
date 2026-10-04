use vstd::prelude::*;
use std::sync::Arc;
use crate::storage::*;
use crate::values::*;
use crate::meaning::*;
use verus_common::sums::*;

verus! {

pub fn add_work_total(work: &mut Work<Vec<Total>>, key: i64, year: i64, count: i128, profit: i128) -> (result: Option<()>)
    requires old(work).valid(),
    ensures final(work).valid(),
        result.is_some() && totals_work_valid(*old(work)) && count > 0 ==> totals_work_valid(*final(work)),
        result.is_some() ==> forall|target: i64, target_year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(*final(work), target, target_year, field) == work_total(*old(work), target, target_year, field)
                + if target == key && target_year == year as int { if field == 0 { count as int } else { profit as int } } else { 0 },
{
    let ghost initial = *work;
    let mut rows = match work.take(key) { Some(rows) => rows, None => Vec::new() };
    let ghost before_rows = rows@;
    proof {
        assert forall|target_year: int, field: int| 0 <= field <= 1 implies
            #[trigger] total_at(before_rows, target_year, field) == work_total(initial, key, target_year, field) by {
            if !initial.rows@.contains_key(key) {
                lemma_sum_empty(before_rows, |row: Total| if row.year == target_year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
            }
        }
    }
    bump_total(&mut rows, year, count, profit)?;
    work.put(key, rows);
    proof {
        assert forall|target: i64, target_year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_total(*work, target, target_year, field) == work_total(initial, target, target_year, field)
                + if target == key && target_year == year as int { if field == 0 { count as int } else { profit as int } } else { 0 } by {
            if target == key {
                assert(total_at(before_rows, target_year, field) == work_total(initial, key, target_year, field));
                assert(total_at(rows@, target_year, field) == total_at(before_rows, target_year, field)
                    + if target_year == year as int { if field == 0 { count as int } else { profit as int } } else { 0 });
            } else { assert(work_total(*work, target, target_year, field) == work_total(initial, target, target_year, field)); }
        }
    }
    Some(())
}

pub fn add_part(work: &mut Work<Vec<Total>>, part: &Part, sign: i128) -> (result: Option<()>)
    requires old(work).valid(),
    ensures final(work).valid(),
        result.is_some() && totals_work_valid(*old(work)) && part.green > 0
            && pairs_positive(part.pairs@) && moments_positive(part.moments@) && sign > 0
            ==> totals_work_valid(*final(work)),
        result.is_some() ==> forall|target: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(*final(work), target, year, field) == work_total(*old(work), target, year, field)
                + part_fold(*part, part.moments@, target, year, field) * sign as int,
{
    let ghost initial = *work;
    proof {
        assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] part_prefix(*part, 0, 0, target, year, field) == 0 by {
            lemma_sum_empty(part.moments@.take(0), |moment: Moment| pair_fold(*part, moment, part.pairs@, target, year, field));
            if part.moments.len() > 0 {
                lemma_sum_empty(part.pairs@.take(0), |pair: Pair| part_term(*part, part.moments[0], pair, target, year, field));
            }
        }
        assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                + part_prefix(*part, 0, 0, target, year, field) * sign as int by {
            assert(0int * sign as int == 0) by (nonlinear_arith);
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < part.moments.len()
        invariant index <= part.moments.len(), processed@ == index as int,
            totals_work_valid(initial) && part.green > 0 && pairs_positive(part.pairs@)
                && moments_positive(part.moments@) && sign > 0 ==> totals_work_valid(*work),
            work.valid(),
            initial == *old(work),
            forall|target: i64, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                    + part_prefix(*part, index as int, 0, target, year, field) * sign as int,
        decreases part.moments.len() - index,
    {
        let moment = &part.moments[index];
        let mut pair_index = 0;
        while pair_index < part.pairs.len()
            invariant pair_index <= part.pairs.len(),
                totals_work_valid(initial) && part.green > 0 && pairs_positive(part.pairs@)
                    && moments_positive(part.moments@) && sign > 0 ==> totals_work_valid(*work),
                work.valid(),
                index < part.moments.len(), *moment == part.moments@[index as int],
                forall|target: i64, year: int, field: int| 0 <= field <= 1 ==>
                    #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                        + part_prefix(*part, index as int, pair_index as int, target, year, field) * sign as int,
            decreases part.pairs.len() - pair_index,
        {
            let pair = &part.pairs[pair_index];
            let ghost previous = *work;
            proof {
                assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
                    #[trigger] work_total(previous, target, year, field) == work_total(initial, target, year, field)
                        + part_prefix(*part, index as int, pair_index as int, target, year, field) * sign as int by {}
            }
            if pair.supplier == moment.supplier {
                let count = part.green.checked_mul(pair.count)?.checked_mul(moment.cell.count)?.checked_mul(sign)?;
                proof {
                    if totals_work_valid(initial) && part.green > 0 && pairs_positive(part.pairs@)
                        && moments_positive(part.moments@) && sign > 0 {
                        let green = part.green as int;
                        let multiplicity = pair.count as int;
                        let lines = moment.cell.count as int;
                        let scale = sign as int;
                        assert(green > 0 && multiplicity > 0 && lines > 0 && scale > 0);
                        assert(green * multiplicity * lines * scale > 0) by (nonlinear_arith)
                            requires green > 0, multiplicity > 0, lines > 0, scale > 0;
                        assert(count > 0);
                    }
                }
                let profit = pair.count.checked_mul(moment.cell.revenue)?.checked_sub(pair.cost.checked_mul(moment.cell.quantity)?)?
                    .checked_mul(part.green)?.checked_mul(sign)?;
                add_work_total(work, moment.supplier, moment.year, count, profit)?;
                proof {
                    assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
                        #[trigger] work_total(*work, target, year, field) == work_total(previous, target, year, field)
                            + part_term(*part, *moment, *pair, target, year, field) * sign as int by {
                        if target == moment.supplier && year == moment.year as int {
                            let amount = if field == 0 { count as int } else { profit as int };
                            let green = part.green as int;
                            let pair_count = pair.count as int;
                            let pair_cost = pair.cost as int;
                            let multiplicity = moment.cell.count as int;
                            let revenue = moment.cell.revenue as int;
                            let quantity = moment.cell.quantity as int;
                            if field == 0 {
                                assert(count as int == ((green * pair_count) * multiplicity) * sign as int);
                                assert(part_term(*part, *moment, *pair, target, year, field) == green * (pair_count * multiplicity));
                                assert(((green * pair_count) * multiplicity) * sign as int == (green * (pair_count * multiplicity)) * sign as int) by (nonlinear_arith);
                            } else {
                                assert(profit as int == ((pair_count * revenue - pair_cost * quantity) * green) * sign as int);
                                assert(part_term(*part, *moment, *pair, target, year, field) == green * (pair_count * revenue - pair_cost * quantity));
                                assert(((pair_count * revenue - pair_cost * quantity) * green) * sign as int == (green * (pair_count * revenue - pair_cost * quantity)) * sign as int) by (nonlinear_arith);
                            }
                            assert(amount == part_term(*part, *moment, *pair, target, year, field) * sign as int);
                        }
                    }
                }
            }
            proof {
                assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
                    #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                        + part_prefix(*part, index as int, pair_index as int + 1, target, year, field) * sign as int by {
                    lemma_part_prefix_step(*part, index as int, pair_index as int, target, year, field);
                    let prior = part_prefix(*part, index as int, pair_index as int, target, year, field);
                    let increment = part_term(*part, *moment, *pair, target, year, field);
                    assert((prior + increment) * sign as int == prior * sign as int + increment * sign as int) by (nonlinear_arith);
                    if pair.supplier != moment.supplier { assert(increment == 0); }
                }
            }
            pair_index += 1;
        }
        proof {
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                    + part_prefix(*part, index as int + 1, 0, target, year, field) * sign as int by {
                lemma_part_prefix_finish(*part, index as int, target, year, field);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(part.moments@.take(index as int) =~= part.moments@); }
    Some(())
}

pub fn add_supplier(work: &mut Work<Vec<Total>>, totals: &Vec<Total>, nations: &Vec<Number>, sign: i128) -> (result: Option<()>)
    requires old(work).valid(),
    ensures final(work).valid(),
        result.is_some() && totals_work_valid(*old(work)) && totals_valid(totals@)
            && totals.len() > 0 && numbers_positive(nations@) && sign > 0
            ==> totals_work_valid(*final(work)),
        result.is_some() ==> forall|target: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(*final(work), target, year, field) == work_total(*old(work), target, year, field)
                + number_at(nations@, target as int) * total_at(totals@, year, field) * sign as int,
{
    let ghost initial = *work;
    let ghost positive = totals_work_valid(initial) && totals_valid(totals@)
        && totals.len() > 0 && numbers_positive(nations@) && sign > 0;
    proof {
        assert forall|target: i64| #[trigger] number_at(nations@.take(0), target as int) == 0 by {
            lemma_sum_empty(nations@.take(0), |row: Number| if row.key == target as int { row.count as int } else { 0 });
        }
        assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                + number_at(nations@.take(0), target as int) * total_at(totals@, year, field) * sign as int by {
            assert(0int * total_at(totals@, year, field) * sign as int == 0) by (nonlinear_arith);
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < nations.len()
        invariant index <= nations.len(), processed@ == index as int,
            positive ==> totals_work_valid(*work),
            positive ==> totals_valid(totals@) && totals.len() > 0 && numbers_positive(nations@) && sign > 0,
            work.valid(),
            initial == *old(work),
            forall|target: i64, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                    + number_at(nations@.take(index as int), target as int) * total_at(totals@, year, field) * sign as int,
        decreases nations.len() - index,
    {
        let nation = nations[index];
        let ghost previous = *work;
        proof {
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] work_total(previous, target, year, field) == work_total(initial, target, year, field)
                    + number_at(nations@.take(index as int), target as int) * total_at(totals@, year, field) * sign as int by {}
        }
        let mut rows = match work.take(nation.key) { Some(rows) => rows, None => Vec::new() };
        let ghost base_rows = rows@;
        proof {
            assert forall|year: int, field: int| 0 <= field <= 1 implies
                #[trigger] total_at(base_rows, year, field) == work_total(previous, nation.key, year, field) by {
                if !previous.rows@.contains_key(nation.key) {
                    lemma_sum_empty(base_rows, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
                }
            }
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(totals@.take(0), year, field) == 0 by {
                lemma_sum_empty(totals@.take(0), |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
            }
            assert forall|year: int, field: int| 0 <= field <= 1 implies
                #[trigger] total_at(rows@, year, field) == total_at(base_rows, year, field)
                    + total_at(totals@.take(0), year, field) * nation.count as int * sign as int by {
                assert(0int * nation.count as int * sign as int == 0) by (nonlinear_arith);
            }
        }
        let mut total_index = 0;
        while total_index < totals.len()
            invariant total_index <= totals.len(),
                positive ==> totals_work_valid(*work),
                positive ==> totals_valid(rows@),
                positive ==> totals_work_valid(previous),
                positive && total_index > 0 ==> rows.len() > 0,
                positive ==> totals_valid(totals@) && totals.len() > 0 && nation.count > 0 && sign > 0,
                work.valid(),
                work.rows@ == previous.rows@.remove(nation.key),
                forall|year: int, field: int| 0 <= field <= 1 ==>
                    #[trigger] total_at(base_rows, year, field) == work_total(previous, nation.key, year, field),
                forall|year: int, field: int| 0 <= field <= 1 ==>
                    #[trigger] total_at(rows@, year, field) == total_at(base_rows, year, field)
                        + total_at(totals@.take(total_index as int), year, field) * nation.count as int * sign as int,
            decreases totals.len() - total_index,
        {
            let total = totals[total_index];
            let count = total.count.checked_mul(nation.count)?.checked_mul(sign)?;
            proof {
                if positive {
                    let multiplicity = total.count as int;
                    let copies = nation.count as int;
                    let scale = sign as int;
                    assert(multiplicity > 0 && copies > 0 && scale > 0);
                    assert(multiplicity * copies * scale > 0) by (nonlinear_arith)
                        requires multiplicity > 0, copies > 0, scale > 0;
                    assert(count > 0);
                }
            }
            let profit = total.profit.checked_mul(nation.count)?.checked_mul(sign)?;
            let ghost previous_rows = rows@;
            proof {
                assert forall|year: int, field: int| 0 <= field <= 1 implies
                    #[trigger] total_at(previous_rows, year, field) == total_at(base_rows, year, field)
                        + total_at(totals@.take(total_index as int), year, field) * nation.count as int * sign as int by {}
            }
            bump_total(&mut rows, total.year, count, profit)?;
            proof {
                assert forall|year: int, field: int| 0 <= field <= 1 implies
                    #[trigger] total_at(rows@, year, field) == total_at(base_rows, year, field)
                        + total_at(totals@.take(total_index as int + 1), year, field) * nation.count as int * sign as int by {
                    let weight = |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 };
                    lemma_sum_take(totals@, total_index as int, weight);
                    let prior = total_at(totals@.take(total_index as int), year, field);
                    let increment = weight(total);
                    assert((prior + increment) * nation.count as int * sign as int == prior * nation.count as int * sign as int + increment * nation.count as int * sign as int) by (nonlinear_arith);
                    assert(total_at(rows@, year, field) == total_at(previous_rows, year, field)
                        + if year == total.year as int { if field == 0 { count as int } else { profit as int } } else { 0 });
                }
            }
            total_index += 1;
        }
        work.put(nation.key, rows);
        proof {
            if positive {
                assert(rows.len() > 0 && totals_valid(rows@));
                assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies {
                    let rows = work.rows@[key];
                    rows.len() > 0 && totals_valid(rows@)
                } by {
                    if key != nation.key { assert(work.rows@[key] == previous.rows@[key]); }
                }
            }
            assert(totals@.take(total_index as int) =~= totals@);
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                    + number_at(nations@.take(index as int + 1), target as int) * total_at(totals@, year, field) * sign as int by {
                lemma_sum_take(nations@, index as int, |row: Number| if row.key == target as int { row.count as int } else { 0 });
                let prior = number_at(nations@.take(index as int), target as int);
                let increment = if nation.key == target { nation.count as int } else { 0int };
                let amount = total_at(totals@, year, field);
                assert((prior + increment) * amount * sign as int == prior * amount * sign as int + increment * amount * sign as int) by (nonlinear_arith);
                assert(amount * nation.count as int * sign as int == nation.count as int * amount * sign as int) by (nonlinear_arith);
                if target == nation.key {
                    assert(work_total(*work, target, year, field) == total_at(rows@, year, field));
                } else { assert(work_total(*work, target, year, field) == work_total(previous, target, year, field)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(nations@.take(index as int) =~= nations@);
        assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_total(*work, target, year, field) == work_total(initial, target, year, field)
                + number_at(nations@, target as int) * total_at(totals@, year, field) * sign as int by {}
    }
    Some(())
}

pub fn copy_groups(groups: &Vec<Group>) -> (copied: Vec<Group>)
    ensures copied.len() == groups.len(),
        forall|position: int| 0 <= position < copied.len() ==> (#[trigger] copied[position]).name@ == groups[position].name@
            && copied[position].totals@ == groups[position].totals@,
        forall|name: Seq<u8>, year: int, field: int| #[trigger] groups_at(copied@, name, year, field) == groups_at(groups@, name, year, field),
        group_entries_unique(groups@) ==> group_entries_unique(copied@),
        forall|key: i64| #[trigger] group_hashes(groups@, key) ==> group_hashes(copied@, key),
{
    let mut result: Vec<Group> = Vec::new();
    let mut index = 0;
    while index < groups.len()
        invariant index <= groups.len(), result.len() == index,
            forall|position: int| 0 <= position < result.len() ==> (#[trigger] result[position]).name@ == groups[position].name@
                && result[position].totals@ == groups[position].totals@,
        decreases groups.len() - index,
    {
        result.push(Group { name: Arc::clone(&groups[index].name), totals: copy_totals(&groups[index].totals) });
        index += 1;
    }
    proof {
        assert forall|name: Seq<u8>, year: int, field: int| #[trigger] groups_at(result@, name, year, field) == groups_at(groups@, name, year, field) by {
            verus_prooflib::lemma_sum_aligned(result@, groups@, group_weight(name, year, field), group_weight(name, year, field));
        }
        assert(result@.map_values(|group: Group| group.name@) =~= groups@.map_values(|group: Group| group.name@));
    }
    result
}

pub fn find_group(groups: &Vec<Group>, name: &Vec<u8>) -> (result: usize)
    ensures result <= groups.len(),
        result < groups.len() ==> groups[result as int].name@ == name@,
        forall|position: int| 0 <= position < result ==> (#[trigger] groups[position]).name@ != name@,
{
    let mut index = 0;
    while index < groups.len()
        invariant index <= groups.len(),
            forall|position: int| 0 <= position < index ==> (#[trigger] groups[position]).name@ != name@,
        decreases groups.len() - index,
    {
        if bytes_equal(&groups[index].name, name) { return index; }
        index += 1;
    }
    index
}

#[verifier::spinoff_prover]
pub fn add_name(work: &mut Work<Vec<Group>>, totals: &Vec<Total>, name: &Name, sign: i128) -> (result: Option<()>)
    requires old(work).valid(),
    ensures final(work).valid(),
        result.is_some() && groups_work_hashed(*old(work)) ==> groups_work_hashed(*final(work)),
        result.is_some() && groups_work_valid(*old(work)) && totals_valid(totals@)
            && totals.len() > 0 && name.count > 0 && sign > 0 ==> groups_work_valid(*final(work)),
        result.is_some() ==> forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_group(*final(work), target, year, field) == work_group(*old(work), target, year, field)
                + if target == name.bytes@ { total_at(totals@, year, field) * name.count as int * sign as int } else { 0int },
{
        broadcast use vstd::seq_lib::group_seq_properties;
        let ghost hashed = groups_work_hashed(*work);
        let ghost positive = groups_work_valid(*work) && totals_valid(totals@)
            && totals.len() > 0 && name.count > 0 && sign > 0;
        let key = name_key(&name.bytes);
        let ghost previous = *work;
        let mut groups = match work.take(key) { Some(groups) => groups, None => Vec::new() };
        let ghost before_groups = groups@;
        proof { if hashed { assert(group_hashes(before_groups, key)); } }
        proof {
            assert forall|target: Seq<u8>, year: int, field: int| name_hash(target) as i64 == key && 0 <= field <= 1 implies
                #[trigger] groups_at(before_groups, target, year, field) == work_group(previous, target, year, field) by {
                if !previous.rows@.contains_key(key) { lemma_sum_empty(before_groups, group_weight(target, year, field)); }
            }
        }
        let group_index = find_group(&groups, &name.bytes);
        let found = group_index < groups.len();
        let mut group = if found { groups.remove(group_index) }
            else { Group { name: Arc::clone(&name.bytes), totals: Vec::new() } };
        let ghost base_totals = group.totals@;
        let ghost remaining = groups@;
        proof {
            if hashed {
                assert forall|position: int| 0 <= position < groups.len() implies name_hash((#[trigger] groups[position]).name@) == key as int by {
                    if position < remaining.len() { assert(groups[position] == remaining[position]); }
                }
                assert forall|target: i64| #[trigger] work.rows@.contains_key(target) implies group_hashes(work.rows@[target]@, target) by {
                    if target != key { assert(work.rows@[target] == previous.rows@[target]); }
                }
            }
            if positive {
                let projected = before_groups.map_values(|group: Group| group.name@);
                if found {
                    assert(remaining.map_values(|group: Group| group.name@) =~= projected.remove(group_index as int));
                    verus_prooflib::lemma_unique_remove(projected, group_index as int);
                }
                assert(remaining.map_values(|group: Group| group.name@).no_duplicates());
                assert forall|position: int| 0 <= position < remaining.len() implies (#[trigger] remaining[position]).name@ != name.bytes@ by {
                    if found {
                        let source = if position < group_index { position } else { position + 1 };
                        assert(remaining[position] == before_groups[source]);
                        assert(source != group_index as int);
                        assert(projected[source] != projected[group_index as int]);
                    }
                }
                assert(!remaining.map_values(|group: Group| group.name@).contains(name.bytes@));
                assert(totals_valid(group.totals@));
            }
            assert(group.name@ == name.bytes@);
            if hashed {
                assert forall|position: int| 0 <= position < remaining.len() implies name_hash((#[trigger] remaining[position]).name@) == key as int by {
                    let source = if found && position >= group_index { position + 1 } else { position };
                    assert(remaining[position] == before_groups[source]);
                }
            }
            assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] groups_at(before_groups, target, year, field) == groups_at(remaining, target, year, field)
                    + if target == name.bytes@ { total_at(base_totals, year, field) } else { 0int } by {
                if found {
                    lemma_sum_remove(before_groups, group_index as int, group_weight(target, year, field));
                    assert(before_groups[group_index as int].name@ == name.bytes@);
                } else { lemma_sum_empty(base_totals, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 }); }
            }
        }
        proof {
            assert forall|year: int, field: int| 0 <= field <= 1 implies
                #[trigger] total_at(totals@.take(0), year, field) == 0 by {
                lemma_sum_empty(totals@.take(0), |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
            }
            assert forall|year: int, field: int| 0 <= field <= 1 implies
                #[trigger] total_at(group.totals@, year, field) == total_at(base_totals, year, field)
                    + total_at(totals@.take(0), year, field) * name.count as int * sign as int by {
                assert(0int * name.count as int * sign as int == 0) by (nonlinear_arith);
            }
        }
        let mut total_index = 0;
        while total_index < totals.len()
            invariant total_index <= totals.len(),
                work.valid(),
                hashed ==> groups_work_hashed(previous) && group_hashes(remaining, key),
                name_hash(name.bytes@) == key as int,
                work.rows@ == previous.rows@.remove(key),
                positive ==> groups_work_valid(previous) && totals_valid(totals@) && totals.len() > 0 && name.count > 0 && sign > 0,
                positive ==> totals_valid(group.totals@),
                positive && total_index > 0 ==> group.totals.len() > 0,
                group.name@ == name.bytes@,
                groups@ == remaining,
                forall|year: int, field: int| 0 <= field <= 1 ==>
                    #[trigger] total_at(group.totals@, year, field) == total_at(base_totals, year, field)
                        + total_at(totals@.take(total_index as int), year, field) * name.count as int * sign as int,
            decreases totals.len() - total_index,
        {
            let total = totals[total_index];
            let count = total.count.checked_mul(name.count)?.checked_mul(sign)?;
            proof {
                if positive {
                    let multiplicity = total.count as int;
                    let copies = name.count as int;
                    let scale = sign as int;
                    assert(multiplicity > 0 && copies > 0 && scale > 0);
                    assert(multiplicity * copies * scale > 0) by (nonlinear_arith)
                        requires multiplicity > 0, copies > 0, scale > 0;
                    assert(count > 0);
                }
            }
            let profit = total.profit.checked_mul(name.count)?.checked_mul(sign)?;
            bump_total(&mut group.totals, total.year, count, profit)?;
            proof {
                assert forall|year: int, field: int| 0 <= field <= 1 implies
                    #[trigger] total_at(group.totals@, year, field) == total_at(base_totals, year, field)
                        + total_at(totals@.take(total_index as int + 1), year, field) * name.count as int * sign as int by {
                    lemma_sum_take(totals@, total_index as int, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
                    let prior = total_at(totals@.take(total_index as int), year, field);
                    let increment = if total.year as int == year { if field == 0 { total.count as int } else { total.profit as int } } else { 0int };
                    assert((prior + increment) * name.count as int * sign as int
                        == prior * name.count as int * sign as int + increment * name.count as int * sign as int) by (nonlinear_arith);
                    if year == total.year as int {
                        if field == 0 { assert(count as int == total.count as int * name.count as int * sign as int); }
                        else { assert(profit as int == total.profit as int * name.count as int * sign as int); }
                    }
                }
            }
            total_index += 1;
        }
        let ghost completed = group;
        groups.push(group);
        work.put(key, groups);
        proof {
            if positive {
                let projected = remaining.map_values(|group: Group| group.name@);
                verus_prooflib::lemma_unique_push(projected, name.bytes@);
                assert(groups@.map_values(|group: Group| group.name@) =~= projected.push(name.bytes@));
                assert forall|position: int| 0 <= position < groups.len() implies {
                    let group = #[trigger] groups[position];
                    name_hash(group.name@) == key as int && group.totals.len() > 0 && totals_valid(group.totals@)
                } by {
                    if position < remaining.len() {
                        let source = if found && position >= group_index { position + 1 } else { position };
                        assert(groups[position] == before_groups[source]);
                    }
                }
                assert(group_bucket_valid(groups@, key));
                assert forall|target: i64| #[trigger] work.rows@.contains_key(target) implies group_bucket_valid(work.rows@[target]@, target) by {
                    if target != key { assert(work.rows@[target] == previous.rows@[target]); }
                }
            }
            assert(totals@.take(total_index as int) =~= totals@);
            assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] work_group(*work, target, year, field) == work_group(previous, target, year, field)
                    + if target == name.bytes@ { total_at(totals@, year, field) * name.count as int * sign as int } else { 0int } by {
                if name_hash(target) as i64 == key {
                    lemma_sum_push(remaining, completed, group_weight(target, year, field));
                    assert(work_group(*work, target, year, field) == groups_at(groups@, target, year, field));
                } else {
                    assert(target != name.bytes@);
                    assert(work_group(*work, target, year, field) == work_group(previous, target, year, field));
                }
            }
        }
    Some(())
}

pub fn add_nation(work: &mut Work<Vec<Group>>, totals: &Vec<Total>, names: &Vec<Name>, sign: i128) -> (result: Option<()>)
    requires old(work).valid(),
    ensures final(work).valid(),
        result.is_some() && groups_work_hashed(*old(work)) ==> groups_work_hashed(*final(work)),
        result.is_some() && groups_work_valid(*old(work)) && totals_valid(totals@)
            && totals.len() > 0 && names_positive(names@) && sign > 0 ==> groups_work_valid(*final(work)),
        result.is_some() ==> forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_group(*final(work), target, year, field) == work_group(*old(work), target, year, field)
                + name_at(names@, target) * total_at(totals@, year, field) * sign as int,
{
    let ghost initial = *work;
    proof {
        assert forall|target: Seq<u8>| #[trigger] name_at(names@.take(0), target) == 0 by {
            lemma_sum_empty(names@.take(0), |row: Name| if row.bytes@ == target { row.count as int } else { 0 });
        }
        assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] work_group(*work, target, year, field) == work_group(initial, target, year, field)
                + name_at(names@.take(0), target) * total_at(totals@, year, field) * sign as int by {
            assert(0int * total_at(totals@, year, field) * sign as int == 0) by (nonlinear_arith);
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < names.len()
        invariant index <= names.len(), processed@ == index as int, work.valid(),
            initial == *old(work),
            groups_work_hashed(initial) ==> groups_work_hashed(*work),
            groups_work_valid(initial) && totals_valid(totals@) && totals.len() > 0
                && names_positive(names@) && sign > 0 ==> groups_work_valid(*work),
            forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] work_group(*work, target, year, field) == work_group(initial, target, year, field)
                    + name_at(names@.take(index as int), target) * total_at(totals@, year, field) * sign as int,
        decreases names.len() - index,
    {
        add_name(work, totals, &names[index], sign)?;
        proof {
            assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] work_group(*work, target, year, field) == work_group(initial, target, year, field)
                    + name_at(names@.take(index as int + 1), target) * total_at(totals@, year, field) * sign as int by {
                lemma_sum_take(names@, index as int, |row: Name| if row.bytes@ == target { row.count as int } else { 0 });
                let prior = name_at(names@.take(index as int), target);
                let increment = if names[index as int].bytes@ == target { names[index as int].count as int } else { 0int };
                let amount = total_at(totals@, year, field);
                assert((prior + increment) * amount * sign as int == prior * amount * sign as int + increment * amount * sign as int) by (nonlinear_arith);
                assert(amount * names[index as int].count as int * sign as int == names[index as int].count as int * amount * sign as int) by (nonlinear_arith);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(names@.take(index as int) =~= names@); }
    Some(())
}

#[verifier::spinoff_prover]
pub fn prune_groups(groups: &mut Vec<Group>) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] groups_at(final(groups)@, name, year, field) == groups_at(old(groups)@, name, year, field),
        group_entries_unique(old(groups)@) ==> group_entries_unique(final(groups)@),
        forall|key: i64| #[trigger] group_hashes(old(groups)@, key) ==> group_hashes(final(groups)@, key),
        result.is_some() ==> forall|position: int| 0 <= position < final(groups).len() ==> {
            let group = #[trigger] final(groups)[position];
            group.totals.len() > 0 && totals_positive(group.totals@)
        },
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = groups@;
    let mut index = 0;
    while index < groups.len()
        invariant index <= groups.len(),
            initial == old(groups)@,
            forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] groups_at(groups@, name, year, field) == groups_at(initial, name, year, field),
            group_entries_unique(initial) ==> group_entries_unique(groups@),
            forall|key: i64| #[trigger] group_hashes(initial, key) ==> group_hashes(groups@, key),
            forall|position: int| 0 <= position < index ==> {
                let group = #[trigger] groups[position];
                group.totals.len() > 0 && totals_positive(group.totals@)
            },
        decreases groups.len() - index,
    {
        let ghost before = groups@;
        let mut group = groups.remove(index);
        let ghost remaining = groups@;
        let ghost old_totals = group.totals@;
        let ghost old_name = group.name@;
        proof {
            if group_entries_unique(initial) {
                let names = before.map_values(|group: Group| group.name@);
                assert(remaining.map_values(|group: Group| group.name@) =~= names.remove(index as int));
                verus_prooflib::lemma_unique_remove(names, index as int);
                assert forall|position: int| 0 <= position < remaining.len() implies
                    (#[trigger] remaining[position]).totals@.map_values(|row: Total| row.year).no_duplicates() by {
                    let source = if position < index { position } else { position + 1 };
                    assert(remaining[position] == before[source]);
                }
            }
            assert forall|key: i64| #[trigger] group_hashes(initial, key) implies group_hashes(remaining, key) by {
                assert forall|position: int| 0 <= position < remaining.len() implies name_hash((#[trigger] remaining[position]).name@) == key as int by {
                    let source = if position < index { position } else { position + 1 };
                    assert(remaining[position] == before[source]);
                }
            }
        }
        prune_totals(&mut group.totals)?;
        let ghost completed = group;
        let keep = group.totals.len() > 0;
        if keep { groups.insert(index, group); }
        proof {
            assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] groups_at(groups@, name, year, field) == groups_at(before, name, year, field) by {
                let weight = group_weight(name, year, field);
                lemma_sum_remove(before, index as int, weight);
                if keep {
                    verus_prooflib::lemma_sum_insert(remaining, index as int, completed, weight);
                } else {
                    lemma_sum_empty(completed.totals@, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
                }
                assert(total_at(old_totals, year, field) == total_at(completed.totals@, year, field));
            }
            if group_entries_unique(initial) {
                if keep {
                    assert(groups@.map_values(|group: Group| group.name@) =~= before.map_values(|group: Group| group.name@));
                }
                assert forall|position: int| 0 <= position < groups.len() implies
                    (#[trigger] groups[position]).totals@.map_values(|row: Total| row.year).no_duplicates() by {
                    if keep && position == index { assert(groups[position] == completed); }
                    else {
                        let source = if keep || position < index { position } else { position + 1 };
                        assert(groups[position] == before[source]);
                    }
                }
            }
            assert forall|key: i64| #[trigger] group_hashes(initial, key) implies group_hashes(groups@, key) by {
                assert forall|position: int| 0 <= position < groups.len() implies name_hash((#[trigger] groups[position]).name@) == key as int by {
                    let source = if keep || position < index { position } else { position + 1 };
                    assert(groups[position].name@ == before[source].name@);
                }
            }
        }
        if keep { index += 1; }
    }
    Some(())
}

pub fn prune_numbers(rows: &mut Vec<Number>) -> (result: Option<()>)
    ensures
        forall|key: int| #[trigger] number_at(final(rows)@, key) == number_at(old(rows)@, key),
        old(rows)@.map_values(|row: Number| row.key).no_duplicates() ==> final(rows)@.map_values(|row: Number| row.key).no_duplicates(),
        result.is_some() ==> numbers_positive(final(rows)@),
        result.is_some() ==> forall|position: int| 0 <= position < final(rows).len() ==> (#[trigger] final(rows)[position]).count <= u64::MAX,
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(),
            initial == old(rows)@,
            forall|key: int| #[trigger] number_at(rows@, key) == number_at(initial, key),
            initial.map_values(|row: Number| row.key).no_duplicates() ==> rows@.map_values(|row: Number| row.key).no_duplicates(),
            forall|position: int| 0 <= position < index ==> 0 < (#[trigger] rows[position]).count <= u64::MAX,
        decreases rows.len() - index,
    {
        if rows[index].count < 0 || rows[index].count > u64::MAX as i128 { return None; }
        if rows[index].count == 0 {
            let ghost before = rows@;
            rows.remove(index);
            proof {
                assert forall|key: int| #[trigger] number_at(rows@, key) == number_at(before, key) by {
                    lemma_sum_remove(before, index as int, |row: Number| if row.key == key { row.count as int } else { 0int });
                }
                if before.map_values(|row: Number| row.key).no_duplicates() {
                    assert(rows@.map_values(|row: Number| row.key) =~= before.map_values(|row: Number| row.key).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.map_values(|row: Number| row.key), index as int);
                }
            }
        } else { index += 1; }
    }
    Some(())
}

pub fn prune_names(rows: &mut Vec<Name>) -> (result: Option<()>)
    ensures
        forall|name: Seq<u8>| #[trigger] name_at(final(rows)@, name) == name_at(old(rows)@, name),
        old(rows)@.map_values(|row: Name| row.bytes@).no_duplicates() ==> final(rows)@.map_values(|row: Name| row.bytes@).no_duplicates(),
        result.is_some() ==> names_positive(final(rows)@),
        result.is_some() ==> forall|position: int| 0 <= position < final(rows).len() ==> (#[trigger] final(rows)[position]).count <= u64::MAX,
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(),
            initial == old(rows)@,
            forall|name: Seq<u8>| #[trigger] name_at(rows@, name) == name_at(initial, name),
            initial.map_values(|row: Name| row.bytes@).no_duplicates() ==> rows@.map_values(|row: Name| row.bytes@).no_duplicates(),
            forall|position: int| 0 <= position < index ==> 0 < (#[trigger] rows[position]).count <= u64::MAX,
        decreases rows.len() - index,
    {
        if rows[index].count < 0 || rows[index].count > u64::MAX as i128 { return None; }
        if rows[index].count == 0 {
            let ghost before = rows@;
            rows.remove(index);
            proof {
                assert forall|name: Seq<u8>| #[trigger] name_at(rows@, name) == name_at(before, name) by {
                    lemma_sum_remove(before, index as int, |row: Name| if row.bytes@ == name { row.count as int } else { 0int });
                }
                if before.map_values(|row: Name| row.bytes@).no_duplicates() {
                    assert(rows@.map_values(|row: Name| row.bytes@) =~= before.map_values(|row: Name| row.bytes@).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.map_values(|row: Name| row.bytes@), index as int);
                }
            }
        } else { index += 1; }
    }
    Some(())
}

pub fn prune_years(rows: &mut Vec<Year>) -> (result: Option<()>)
    ensures
        forall|year: int| #[trigger] year_at(final(rows)@, year) == year_at(old(rows)@, year),
        old(rows)@.map_values(|row: Year| row.year).no_duplicates() ==> final(rows)@.map_values(|row: Year| row.year).no_duplicates(),
        result.is_some() ==> years_positive(final(rows)@),
        result.is_some() ==> forall|position: int| 0 <= position < final(rows).len() ==> (#[trigger] final(rows)[position]).count <= u64::MAX,
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(),
            initial == old(rows)@,
            forall|year: int| #[trigger] year_at(rows@, year) == year_at(initial, year),
            initial.map_values(|row: Year| row.year).no_duplicates() ==> rows@.map_values(|row: Year| row.year).no_duplicates(),
            forall|position: int| 0 <= position < index ==> 0 < (#[trigger] rows[position]).count <= u64::MAX,
        decreases rows.len() - index,
    {
        if rows[index].count < 0 || rows[index].count > u64::MAX as i128 { return None; }
        if rows[index].count == 0 {
            let ghost before = rows@;
            rows.remove(index);
            proof {
                assert forall|year: int| #[trigger] year_at(rows@, year) == year_at(before, year) by {
                    lemma_sum_remove(before, index as int, |row: Year| if row.year == year { row.count as int } else { 0int });
                }
                if before.map_values(|row: Year| row.year).no_duplicates() {
                    assert(rows@.map_values(|row: Year| row.year) =~= before.map_values(|row: Year| row.year).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.map_values(|row: Year| row.year), index as int);
                }
            }
        } else { index += 1; }
    }
    Some(())
}

pub fn row_less(left: &(Vec<u8>, i64, i128), right: &(Vec<u8>, i64, i128)) -> (result: bool)
    ensures result == crate::gen_spec::row_lt(crate::output::row_view(*left), crate::output::row_view(*right)),
{
    bytes_less(&left.0, &right.0) || (bytes_equal(&left.0, &right.0) && left.1 > right.1)
}

pub fn merge_totals(rows: &mut Vec<Total>, changes: &Vec<Total>) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] total_at(final(rows)@, year, field) == total_at(old(rows)@, year, field) + total_at(changes@, year, field),
        old(rows)@.map_values(|row: Total| row.year).no_duplicates() ==> final(rows)@.map_values(|row: Total| row.year).no_duplicates(),
{
    let ghost initial = rows@;
    proof {
        assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(changes@.take(0), year, field) == 0 by {
            lemma_sum_empty(changes@.take(0), |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int,
            initial == old(rows)@,
            forall|year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] total_at(rows@, year, field) == total_at(initial, year, field) + total_at(changes@.take(processed@), year, field),
            initial.map_values(|row: Total| row.year).no_duplicates() ==> rows@.map_values(|row: Total| row.year).no_duplicates(),
        decreases changes.len() - index,
    {
        let change = changes[index];
        bump_total(rows, change.year, change.count, change.profit)?;
        proof {
            assert forall|year: int, field: int| 0 <= field <= 1 implies
                #[trigger] total_at(rows@, year, field) == total_at(initial, year, field) + total_at(changes@.take(index as int + 1), year, field) by {
                lemma_sum_take(changes@, index as int, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(changes@.take(processed@) =~= changes@); }
    Some(())
}

#[verifier::spinoff_prover]
pub fn merge_groups(groups: &mut Vec<Group>, changes: &Vec<Group>) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] groups_at(final(groups)@, name, year, field) == groups_at(old(groups)@, name, year, field) + groups_at(changes@, name, year, field),
        group_entries_unique(old(groups)@) ==> group_entries_unique(final(groups)@),
        forall|key: i64| #[trigger] group_hashes(old(groups)@, key) && group_hashes(changes@, key) ==> group_hashes(final(groups)@, key),
        result.is_some() ==> forall|position: int| 0 <= position < final(groups).len() ==> {
            let group = #[trigger] final(groups)[position];
            group.totals.len() > 0 && totals_positive(group.totals@)
        },
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = groups@;
    proof {
        assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] groups_at(groups@, name, year, field) == groups_at(initial, name, year, field) + groups_at(changes@.take(0), name, year, field) by {
            lemma_sum_empty(changes@.take(0), group_weight(name, year, field));
        }
    }
    let mut index = 0;
    while index < changes.len()
        invariant index <= changes.len(),
            initial == old(groups)@,
            forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] groups_at(groups@, name, year, field) == groups_at(initial, name, year, field) + groups_at(changes@.take(index as int), name, year, field),
            group_entries_unique(initial) ==> group_entries_unique(groups@),
            forall|key: i64| #[trigger] group_hashes(initial, key) && group_hashes(changes@, key) ==> group_hashes(groups@, key),
        decreases changes.len() - index,
    {
        let change = &changes[index];
        let ghost before = groups@;
        proof {
            assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] groups_at(before, name, year, field) == groups_at(initial, name, year, field) + groups_at(changes@.take(index as int), name, year, field) by {}
        }
        let target = find_group(groups, &change.name);
        let found = target < groups.len();
        let mut group = if found { groups.remove(target) }
            else { Group { name: Arc::clone(&change.name), totals: Vec::new() } };
        let ghost remaining = groups@;
        let ghost base_totals = group.totals@;
        proof {
            assert(group.name@ == change.name@);
            if group_entries_unique(initial) {
                let names = before.map_values(|group: Group| group.name@);
                if found {
                    assert(remaining.map_values(|group: Group| group.name@) =~= names.remove(target as int));
                    verus_prooflib::lemma_unique_remove(names, target as int);
                }
                assert(base_totals.map_values(|row: Total| row.year).no_duplicates());
                assert forall|position: int| 0 <= position < remaining.len() implies (#[trigger] remaining[position]).name@ != change.name@
                    && remaining[position].totals@.map_values(|row: Total| row.year).no_duplicates() by {
                    let source = if found && position >= target { position + 1 } else { position };
                    assert(remaining[position] == before[source]);
                    if found { assert(names[source] != names[target as int]); }
                }
                assert(!remaining.map_values(|group: Group| group.name@).contains(change.name@));
            }
            assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] groups_at(before, name, year, field) == groups_at(remaining, name, year, field)
                    + if name == change.name@ { total_at(base_totals, year, field) } else { 0int } by {
                if found { lemma_sum_remove(before, target as int, group_weight(name, year, field)); }
                else { lemma_sum_empty(base_totals, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int }); }
            }
            assert forall|key: i64| #[trigger] group_hashes(initial, key) && group_hashes(changes@, key) implies group_hashes(remaining, key) by {
                assert forall|position: int| 0 <= position < remaining.len() implies name_hash((#[trigger] remaining[position]).name@) == key as int by {
                    let source = if found && position >= target { position + 1 } else { position };
                    assert(remaining[position] == before[source]);
                }
            }
        }
        merge_totals(&mut group.totals, &change.totals)?;
        let ghost completed = group;
        proof {
            assert(completed.name@ == change.name@);
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(completed.totals@, year, field)
                == total_at(base_totals, year, field) + total_at(change.totals@, year, field) by {}
        }
        groups.push(group);
        proof {
            if group_entries_unique(initial) {
                verus_prooflib::lemma_unique_push(remaining.map_values(|group: Group| group.name@), change.name@);
                assert(groups@.map_values(|group: Group| group.name@) =~= remaining.map_values(|group: Group| group.name@).push(change.name@));
                assert forall|position: int| 0 <= position < groups.len() implies (#[trigger] groups[position]).totals@.map_values(|row: Total| row.year).no_duplicates() by {
                    if position < remaining.len() { assert(groups[position] == remaining[position]); }
                    else { assert(groups[position] == completed); }
                }
            }
            assert forall|key: i64| #[trigger] group_hashes(initial, key) && group_hashes(changes@, key) implies group_hashes(groups@, key) by {
                assert forall|position: int| 0 <= position < groups.len() implies name_hash((#[trigger] groups[position]).name@) == key as int by {
                    if position < remaining.len() { assert(groups[position] == remaining[position]); }
                    else { assert(groups[position].name@ == changes[index as int].name@); }
                }
            }
            assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] groups_at(groups@, name, year, field) == groups_at(initial, name, year, field) + groups_at(changes@.take(index as int + 1), name, year, field) by {
                lemma_sum_push(remaining, completed, group_weight(name, year, field));
                lemma_sum_take(changes@, index as int, group_weight(name, year, field));
                assert(groups_at(groups@, name, year, field) == groups_at(remaining, name, year, field) + group_weight(name, year, field)(completed));
                assert(groups_at(changes@.take(index as int + 1), name, year, field) == groups_at(changes@.take(index as int), name, year, field) + group_weight(name, year, field)(changes[index as int]));
                assert(groups_at(before, name, year, field) == groups_at(remaining, name, year, field) + if name == change.name@ { total_at(base_totals, year, field) } else { 0int });
            }
        }
        index += 1;
    }
    proof { assert(changes@.take(index as int) =~= changes@); }
    prune_groups(groups)
}

pub fn sort_rows(rows: Vec<(Vec<u8>, i64, i128)>) -> (result: Vec<(Vec<u8>, i64, i128)>)
    requires crate::gen_obligations::out_view(rows@).map_values(|row: crate::gen_spec::OutRow| crate::output::output_key(row)).no_duplicates(),
    ensures
        forall|row: crate::gen_spec::OutRow| #[trigger] crate::gen_obligations::out_view(result@).contains(row)
            <==> crate::gen_obligations::out_view(rows@).contains(row),
        forall|left: int, right: int| 0 <= left < right < result.len() ==>
            !crate::gen_spec::same_group(#[trigger] crate::gen_obligations::out_view(result@)[left], #[trigger] crate::gen_obligations::out_view(result@)[right])
                && !crate::gen_spec::row_lt(crate::gen_obligations::out_view(result@)[right], crate::gen_obligations::out_view(result@)[left]),
{
    let ghost views = crate::gen_obligations::out_view(rows@);
    let mut indices: Vec<usize> = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), indices.len() == index,
            forall|position: int| 0 <= position < indices.len() ==> (#[trigger] indices@[position]) as int == position,
        decreases rows.len() - index,
    {
        indices.push(index);
        index += 1;
    }
    let ghost valid = |position: usize| position < rows.len();
    let ghost le = |left: usize, right: usize| !crate::gen_spec::row_lt(crate::output::row_view(rows@[right as int]), crate::output::row_view(rows@[left as int]));
    let rows_ref = &rows;
    let compare = |left: usize, right: usize| -> (result: bool)
        requires left < rows_ref.len(), right < rows_ref.len(),
        ensures result == !crate::gen_spec::row_lt(crate::output::row_view(rows_ref@[right as int]), crate::output::row_view(rows_ref@[left as int])),
    { !row_less(&rows_ref[right], &rows_ref[left]) };
    proof {
        assert forall|left: usize, right: usize, last: usize| valid(left) && valid(right) && valid(last)
            && #[trigger] le(left, right) && #[trigger] le(right, last) implies le(left, last) by {
            crate::output::lemma_row_preorder(views[left as int], views[right as int], views[last as int]);
        }
        assert forall|left: usize, right: usize| valid(left) && valid(right) implies #[trigger] le(left, right) || le(right, left) by {
            verus_common::bytes::lemma_bytes_lt_asym(rows@[left as int].0@, rows@[right as int].0@);
        }
    }
    let sorted = verus_common::sort::sort_idx(&indices, &compare, Ghost(le), Ghost(valid));
    proof { crate::output::lemma_index_permutation(sorted@, indices@, rows.len() as nat); }
    let mut result = Vec::new();
    index = 0;
    while index < sorted.len()
        invariant index <= sorted.len(), result.len() == index, views == crate::gen_obligations::out_view(rows@),
            forall|position: int| 0 <= position < sorted.len() ==> (#[trigger] sorted@[position]) < rows.len(),
            sorted@.no_duplicates(), verus_common::sort::sorted_le(sorted@, le),
            forall|position: usize| position < rows.len() ==> #[trigger] sorted@.contains(position),
            views.map_values(|row: crate::gen_spec::OutRow| crate::output::output_key(row)).no_duplicates(),
            forall|position: int| 0 <= position < result.len() ==>
                (#[trigger] crate::gen_obligations::out_view(result@)[position]) == views[sorted@[position] as int],
        decreases sorted.len() - index,
    {
        let ghost before = crate::gen_obligations::out_view(result@);
        let row = &rows[sorted[index]];
        result.push((copy_bytes(&row.0), row.1, row.2));
        proof {
            assert(crate::gen_obligations::out_view(result@) =~= before.push(views[sorted@[index as int] as int]));
            assert forall|position: int| 0 <= position < result.len() implies
                (#[trigger] crate::gen_obligations::out_view(result@)[position]) == views[sorted@[position] as int] by {
                if position < index as int { assert(before[position] == views[sorted@[position] as int]); }
            }
        }
        index += 1;
    }
    proof {
        assert forall|row: crate::gen_spec::OutRow| #[trigger] crate::gen_obligations::out_view(result@).contains(row)
            <==> views.contains(row) by {
            if crate::gen_obligations::out_view(result@).contains(row) {
                let position = choose|position: int| 0 <= position < result.len() && (#[trigger] crate::gen_obligations::out_view(result@)[position]) == row;
                assert(views[sorted@[position] as int] == row);
                assert(views.contains(row));
            }
            if views.contains(row) {
                let position = choose|position: int| 0 <= position < views.len() && (#[trigger] views[position]) == row;
                assert(sorted@.contains(position as usize));
                let target = choose|target: int| 0 <= target < sorted.len() && (#[trigger] sorted@[target]) == position as usize;
                assert(crate::gen_obligations::out_view(result@)[target] == row);
            }
        }
        assert forall|left: int, right: int| 0 <= left < right < result.len() implies
            !crate::gen_spec::same_group(#[trigger] crate::gen_obligations::out_view(result@)[left], #[trigger] crate::gen_obligations::out_view(result@)[right])
                && !crate::gen_spec::row_lt(crate::gen_obligations::out_view(result@)[right], crate::gen_obligations::out_view(result@)[left]) by {
            assert(sorted@[left] != sorted@[right]);
            assert(views.map_values(|row: crate::gen_spec::OutRow| crate::output::output_key(row))[sorted@[left] as int]
                != views.map_values(|row: crate::gen_spec::OutRow| crate::output::output_key(row))[sorted@[right] as int]);
            assert(le(sorted@[left], sorted@[right]));
        }
    }
    result
}

}
