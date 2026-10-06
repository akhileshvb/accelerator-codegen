use vstd::prelude::*;
use crate::paged::{Paged, PageCursor, entry_sorted, Ent};
use crate::rank_ent::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key};
use crate::model::*;
use crate::values::*;
use crate::gen_obligations::seq_view_i64;
use verus_prooflib::ordered::KeyView;

verus! {

pub fn append_groups(ranks: &mut Index<Paged<RankEnt>>, groups: &Vec<GroupCell>)
    requires ranks_wf(*old(ranks)),
        forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> groups@[position].bound >= 0 && id_fits(groups@[position].id),
    ensures ranks_wf(*final(ranks)),
        forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*final(ranks), outer, rank)]
            rank_at(*final(ranks), outer, rank) == (rank_at(*old(ranks), outer, rank) || rank_present(groups@, outer, rank)),
{
    let ghost original = *ranks;
    let mut position = 0;
    while position < groups.len()
        invariant position <= groups.len(), ranks_wf(*ranks),
            forall|source: int| #![trigger groups@[source]] 0 <= source < groups@.len() ==> groups@[source].bound >= 0 && id_fits(groups@[source].id),
            forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*ranks, outer, rank)]
                rank_at(*ranks, outer, rank) == (rank_at(original, outer, rank) || rank_present(groups@.take(position as int), outer, rank)),
        decreases groups.len() - position,
    {
        let ghost before = *ranks;
        change(ranks, &groups[position], true);
        proof {
            assert forall|outer: KeyView, rank: KeyView| #![trigger rank_present(groups@.take(position as int + 1), outer, rank)]
                rank_present(groups@.take(position as int + 1), outer, rank) ==
                    (rank_present(groups@.take(position as int), outer, rank)
                        || ((*groups@[position as int].segment)@ == outer.1 && covers(groups@[position as int], outer.0)
                            && group_rank(groups@[position as int]) == rank)) by {
                if rank_present(groups@.take(position as int + 1), outer, rank) {
                    let source = choose|source: int| #![trigger groups@.take(position as int + 1)[source]]
                        0 <= source < position + 1 && (*groups@[source].segment)@ == outer.1
                        && covers(groups@[source], outer.0) && group_rank(groups@[source]) == rank;
                    if source < position { assert(groups@.take(position as int)[source] == groups@[source]); }
                }
                if rank_present(groups@.take(position as int), outer, rank) {
                    let source = choose|source: int| #![trigger groups@.take(position as int)[source]]
                        0 <= source < position && (*groups@[source].segment)@ == outer.1
                        && covers(groups@[source], outer.0) && group_rank(groups@[source]) == rank;
                    assert(groups@.take(position as int + 1)[source] == groups@[source]);
                }
                assert(groups@.take(position as int + 1)[position as int] == groups@[position as int]);
            }
            assert forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*ranks, outer, rank)]
                rank_at(*ranks, outer, rank) == (rank_at(original, outer, rank) || rank_present(groups@.take(position as int + 1), outer, rank)) by {
                assert(rank_at(before, outer, rank) == (rank_at(original, outer, rank) || rank_present(groups@.take(position as int), outer, rank)));
            }
        }
        position += 1;
    }
    proof { assert(groups@.take(position as int) =~= groups@); }
}

pub proof fn lemma_rank_concat(left: Seq<GroupCell>, right: Seq<GroupCell>, outer: KeyView, rank: KeyView)
    ensures rank_present(left + right, outer, rank) == (rank_present(left, outer, rank) || rank_present(right, outer, rank)),
{
    if rank_present(left + right, outer, rank) {
        let source = choose|source: int| #![trigger (left + right)[source]] 0 <= source < (left + right).len()
            && (*((left + right)[source]).segment)@ == outer.1 && covers((left + right)[source], outer.0)
            && group_rank((left + right)[source]) == rank;
        if source < left.len() { assert(left[source] == (left + right)[source]); }
        else { assert(right[source - left.len()] == (left + right)[source]); }
    }
    if rank_present(left, outer, rank) {
        let source = choose|source: int| #![trigger left[source]] 0 <= source < left.len()
            && (*left[source].segment)@ == outer.1 && covers(left[source], outer.0) && group_rank(left[source]) == rank;
        assert((left + right)[source] == left[source]);
    }
    if rank_present(right, outer, rank) {
        let source = choose|source: int| #![trigger right[source]] 0 <= source < right.len()
            && (*right[source].segment)@ == outer.1 && covers(right[source], outer.0) && group_rank(right[source]) == rank;
        assert((left + right)[left.len() + source] == right[source]);
    }
}

pub fn cover(group: &GroupCell) -> (nodes: Vec<i64>)
    ensures forall|index: int| #![trigger nodes@[index]] 0 <= index < nodes@.len() ==> covers(*group, nodes@[index] as int),
        forall|node: i64| #![trigger covers(*group, node as int)] covers(*group, node as int) == nodes@.contains(node),
{
    let mut nodes = Vec::new();
    let last = match group.last_ship { Some(value) => value, None => return nodes };
    let first_date = group.id.date as i64 + 1;
    let last_date = last as i64 - 1;
    if first_date > last_date { return nodes; }
    let mut low = (first_date - i32::MIN as i64) / 128;
    let mut high = (last_date - i32::MIN as i64) / 128 + 1;
    let ghost first = low as int;
    let ghost end = high as int;
    let mut level: i64 = 0;
    while low < high
        invariant
            0 <= level <= 27, 0 <= low <= high <= 67108864,
            group.last_ship == Some(last), first_date == group.id.date as int + 1, last_date == last as int - 1,
            first_date <= last_date,
            first == (first_date - i32::MIN as int) / 128,
            end == (last_date - i32::MIN as int) / 128 + 1,
            0 <= first < end <= 67108864,
            low * pow2(level as int) >= first, (low - 1) * pow2(level as int) < first,
            high * pow2(level as int) <= end, (high + 1) * pow2(level as int) > end,
            forall|index: int| #![trigger nodes@[index]] 0 <= index < nodes@.len() ==> covers(*group, nodes@[index] as int),
            forall|node: i64| #![trigger covers(*group, node as int)] covers(*group, node as int) && node as int / 67108864 < level ==>
                nodes@.contains(node),
        decreases high - low,
    {
        proof {
            verus_prooflib::lemma_dyadic_width_bounds(level as int);
            if level == 27 {
                assert(high == 0) by (nonlinear_arith)
                    requires 0 <= high, high as int * 134217728 <= end, end <= 67108864;
            }
            assert(level < 27);
        }
        let ghost width = pow2(level as int);
        let ghost previous_low = low as int;
        let ghost previous_high = high as int;
        let ghost previous_nodes = nodes@;
        if low % 2 == 1 {
            proof {
                assert(low < 67108864);
                assert((level * 67108864 + low) as int / 67108864 == level as int);
                assert((level * 67108864 + low) as int % 67108864 == low as int);
                assert(((low as int) / 2) * (2 * width) == (low as int - 1) * width) by (nonlinear_arith)
                    requires low as int == 2 * ((low as int) / 2) + 1;
                assert((low as int + 1) * width <= end) by (nonlinear_arith)
                    requires low < high, width > 0, high as int * width <= end;
                assert(covers(*group, (level * 67108864 + low) as int));
            }
            let ghost before_push = nodes@;
            nodes.push(level * 67108864 + low);
            proof { verus_prooflib::lemma_push_membership(before_push, (level as int * 67108864 + low as int) as i64); }
            low += 1;
        }
        proof {
            assert(low % 2 == 0);
            assert(low as int * width >= first) by (nonlinear_arith)
                requires low as int >= previous_low, previous_low * width >= first, width > 0;
        }
        if high % 2 == 1 {
            proof { assert(low < high); }
            high -= 1;
            proof {
                assert(high < 67108864);
                assert((level * 67108864 + high) as int / 67108864 == level as int);
                assert((level * 67108864 + high) as int % 67108864 == high as int);
                assert(((high as int) / 2 + 1) * (2 * width) == (high as int + 2) * width) by (nonlinear_arith)
                    requires high as int == 2 * ((high as int) / 2);
                assert(high as int * width >= first) by (nonlinear_arith)
                    requires high >= low, width > 0, low as int * width >= first;
                assert(covers(*group, (level * 67108864 + high) as int));
            }
            let ghost before_push = nodes@;
            nodes.push(level * 67108864 + high);
            proof { verus_prooflib::lemma_push_membership(before_push, (level as int * 67108864 + high as int) as i64); }
        }
        proof {
            assert forall|node: i64| #![trigger covers(*group, node as int)] covers(*group, node as int) && node as int / 67108864 < level + 1 implies
                nodes@.contains(node) by {
                let node_level = node as int / 67108864;
                let offset = node as int % 67108864;
                if node_level < level { assert(previous_nodes.contains(node)); }
                else {
                    assert(node_level == level as int);
                    assert(node as int == level as int * 67108864 + offset);
                    if level == 26 {
                        assert(width == 67108864);
                        assert((offset / 2 + 1) * (2 * width) > end) by (nonlinear_arith)
                            requires width == 67108864, offset / 2 >= 0, end <= 67108864;
                    }
                    verus_prooflib::lemma_dyadic_boundary(first, end, width, previous_low, previous_high, offset);
                    if offset == previous_low && previous_low % 2 == 1 {
                        assert(nodes@.contains((level as int * 67108864 + previous_low) as i64));
                    } else {
                        assert(offset == previous_high - 1 && previous_high % 2 == 1);
                        assert(nodes@.contains((level as int * 67108864 + previous_high - 1) as i64));
                    }
                }
            }
        }
        let ghost padded_low = low as int;
        let ghost padded_high = high as int;
        low = low / 2;
        high = high / 2;
        level += 1;
        proof {
            assert(padded_low == 2 * low as int);
            assert(padded_high == 2 * high as int);
            assert(pow2(level as int) == 2 * width);
            assert(low as int * pow2(level as int) >= first) by (nonlinear_arith)
                requires padded_low == 2 * low as int, padded_low >= previous_low, width > 0,
                    previous_low * width >= first, pow2(level as int) == 2 * width;
            assert((low as int - 1) * pow2(level as int) < first) by (nonlinear_arith)
                requires padded_low == 2 * low as int, padded_low <= previous_low + 1, width > 0,
                    (previous_low - 1) * width < first, pow2(level as int) == 2 * width;
            assert(high as int * pow2(level as int) <= end) by (nonlinear_arith)
                requires padded_high == 2 * high as int, padded_high <= previous_high, width > 0,
                    previous_high * width <= end, pow2(level as int) == 2 * width;
            assert((high as int + 1) * pow2(level as int) > end) by (nonlinear_arith)
                requires padded_high == 2 * high as int, padded_high >= previous_high - 1, width > 0,
                    (previous_high + 1) * width > end, pow2(level as int) == 2 * width;
            assert((high as int - low as int) < previous_high - previous_low);
        }
    }
    proof {
        assert forall|node: i64| #![trigger covers(*group, node as int)] covers(*group, node as int) == nodes@.contains(node) by {
            if covers(*group, node as int) {
                let node_level = node as int / 67108864;
                if node_level >= level {
                    verus_prooflib::lemma_dyadic_width_bounds(node_level);
                    assert(node_level <= 26);
                    assert(first <= (node as int % 67108864) * pow2(node_level));
                    assert(((node as int % 67108864) + 1) * pow2(node_level) <= end);
                    if node_level > level {
                        verus_prooflib::lemma_dyadic_width_grows(level as int, node_level);
                        assert(end - first < 2 * pow2(level as int)) by (nonlinear_arith)
                            requires low == high, (low as int - 1) * pow2(level as int) < first,
                                (high as int + 1) * pow2(level as int) > end;
                        assert(pow2(node_level) <= end - first) by (nonlinear_arith)
                            requires first <= (node as int % 67108864) * pow2(node_level),
                                ((node as int % 67108864) + 1) * pow2(node_level) <= end;
                    }
                    assert(low <= node as int % 67108864 && node as int % 67108864 < high) by (nonlinear_arith)
                        requires node_level == level as int, pow2(node_level) > 0,
                            (low as int - 1) * pow2(level as int) < first,
                            (high as int + 1) * pow2(level as int) > end,
                            first <= (node as int % 67108864) * pow2(node_level),
                            ((node as int % 67108864) + 1) * pow2(node_level) <= end;
                }
            } else if nodes@.contains(node) {
                let position = choose|position: int| #![trigger nodes@[position]] 0 <= position < nodes@.len() && nodes@[position] == node;
            }
        }
    }
    nodes
}

pub fn change(ranks: &mut Index<Paged<RankEnt>>, group: &GroupCell, insert: bool)
    requires group.bound >= 0, id_fits(group.id), ranks_wf(*old(ranks)),
    ensures ranks_wf(*final(ranks)),
        forall|outer: KeyView, key: KeyView| #![trigger rank_at(*final(ranks), outer, key)]
        rank_at(*final(ranks), outer, key) ==
            if outer.1 == (*group.segment)@ && covers(*group, outer.0) && key == group_rank(*group) { insert }
            else { rank_at(*old(ranks), outer, key) },
{
    let ghost original = *ranks;
    let nodes = cover(group);
    let ent = rank_ent(group.bound, group.id);
    let mut index = 0;
    while index < nodes.len()
        invariant index <= nodes.len(), group.bound >= 0, id_fits(group.id),
            ent.kv() == group_rank(*group),
            ranks_wf(*ranks),
            forall|outer: KeyView, key: KeyView| #![trigger rank_at(*ranks, outer, key)]
                rank_at(*ranks, outer, key) ==
                    if outer.1 == (*group.segment)@ && seq_view_i64(nodes@.take(index as int)).contains(outer.0) && key == group_rank(*group) { insert }
                    else { rank_at(original, outer, key) },
        decreases nodes.len() - index,
    {
        let ghost before = *ranks;
        let outer_key = Key { number: nodes[index], text: group.segment.clone() };
        let mut inner = match ranks.get(&outer_key) {
            Some(value) => {
                proof { assert(ranks.at(outer_key.view()) == Some(*value)); }
                value.share()
            },
            None => Paged::new(),
        };
        let ghost before_inner = inner;
        if insert { inner.put(ent); }
        else { inner.remove(&ent); }
        let empty = inner.is_empty();
        if empty { ranks.remove(&outer_key); }
        else { ranks.put(outer_key, Arc::new(inner)); }
        proof {
            assert forall|outer: KeyView, query: KeyView| #![trigger rank_at(*ranks, outer, query)]
                rank_at(*ranks, outer, query) ==
                    if outer == outer_key.view() && query == ent.kv() { insert } else { rank_at(before, outer, query) } by {
                if outer == outer_key.view() {
                    assert(rank_at(before, outer, query) == before_inner.at(query).is_some());
                    if empty { assert(!inner.nonempty()); assert(inner.at(query).is_none()); }
                }
            }
            assert forall|outer: KeyView| #![trigger ranks.at(outer)] ranks.at(outer).is_some() implies ranks.at(outer).unwrap().wf() by {
                if outer == outer_key.view() { assert(ranks.at(outer) == Some(inner)); }
                else { assert(before.at(outer) == ranks.at(outer)); }
            }
            let prior_nodes = seq_view_i64(nodes@.take(index as int));
            assert(seq_view_i64(nodes@.take(index as int + 1)) =~= prior_nodes.push(nodes@[index as int] as int));
            verus_prooflib::lemma_push_membership(prior_nodes, nodes@[index as int] as int);
        }
        index += 1;
    }
    proof {
        assert(nodes@.take(index as int) =~= nodes@);
        assert forall|node: int| #![trigger covers(*group, node)] covers(*group, node) == seq_view_i64(nodes@).contains(node) by {
            if covers(*group, node) {
                assert(0 <= node / 67108864 <= 26);
                assert(0 <= node % 67108864 < 67108864);
                assert(0 <= node < 27 * 67108864);
                let encoded = node as i64;
                assert(encoded as int == node);
                assert(nodes@.contains(encoded));
                let position = choose|position: int| #![trigger nodes@[position]] 0 <= position < nodes@.len() && nodes@[position] == encoded;
                assert(seq_view_i64(nodes@)[position] == node);
            } else if seq_view_i64(nodes@).contains(node) {
                let position = choose|position: int| #![trigger seq_view_i64(nodes@)[position]] 0 <= position < nodes@.len() && seq_view_i64(nodes@)[position] == node;
                assert(covers(*group, nodes@[position] as int));
            }
        }
    }
}

}
