use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::values::*;

verus! {

pub open spec fn page_rank(pages: Index<Page>, outer: KeyView, rank: KeyView) -> bool {
    exists|key: KeyView| #![trigger pages.at(key)] pages.at(key).is_some()
        && rank_present((*pages.at(key).unwrap().groups)@, outer, rank)
}

pub open spec fn rank_exact(pages: Index<Page>, ranks: Index<Index<()>>) -> bool {
    &&& ranks_wf(ranks)
    &&& forall|outer: KeyView, rank: KeyView| #![trigger rank_at(ranks, outer, rank)]
        rank_at(ranks, outer, rank) == page_rank(pages, outer, rank)
}

pub proof fn lemma_rank_share(before: Index<Page>, prior: Index<Index<()>>, after: Index<Page>, ranks: Index<Index<()>>)
    requires rank_exact(before, prior), ranks.wf(),
        forall|key: KeyView| #![trigger after.at(key)] after.at(key) == before.at(key),
        forall|key: KeyView| #![trigger ranks.at(key)] ranks.at(key) == prior.at(key),
    ensures rank_exact(after, ranks),
{
    assert forall|outer: KeyView, rank: KeyView| #![trigger rank_at(ranks, outer, rank)]
        rank_at(ranks, outer, rank) == page_rank(after, outer, rank) by {
        assert(rank_at(ranks, outer, rank) == rank_at(prior, outer, rank));
        if page_rank(before, outer, rank) {
            let source = choose|source: KeyView| #![trigger before.at(source)] before.at(source).is_some() && rank_present((*before.at(source).unwrap().groups)@, outer, rank);
            assert(after.at(source) == before.at(source));
            assert(page_rank(after, outer, rank));
        }
        if page_rank(after, outer, rank) {
            let source = choose|source: KeyView| #![trigger after.at(source)] after.at(source).is_some() && rank_present((*after.at(source).unwrap().groups)@, outer, rank);
            assert(before.at(source) == after.at(source));
            assert(page_rank(before, outer, rank));
        }
    }
}

pub open spec fn page_rank_shape(pages: Index<Page>) -> bool {
    forall|key: KeyView| #![trigger pages.at(key)] pages.at(key).is_some() ==> {
        &&& key.1.len() == 0
        &&& forall|position: int| #![trigger (*pages.at(key).unwrap().groups)@[position]]
            0 <= position < (*pages.at(key).unwrap().groups)@.len() ==>
                (*pages.at(key).unwrap().groups)@[position].id.key as int / 256 == key.0
                    && (*pages.at(key).unwrap().groups)@[position].bound >= 0
    }
}

pub proof fn lemma_rank_exact(pages: Index<Page>, ranks: Index<Index<()>>)
    requires rank_database_ok(pages, ranks),
    ensures rank_exact(pages, ranks),
{
    assert forall|outer: KeyView, rank: KeyView| #![trigger rank_at(ranks, outer, rank)]
        rank_at(ranks, outer, rank) == page_rank(pages, outer, rank) by {
        if page_rank(pages, outer, rank) {
            let key = choose|key: KeyView| #![trigger pages.at(key)] pages.at(key).is_some()
                && rank_present((*pages.at(key).unwrap().groups)@, outer, rank);
            let groups = (*pages.at(key).unwrap().groups)@;
            let position = choose|position: int| #![trigger groups[position]] 0 <= position < groups.len()
                && (*groups[position].segment)@ == outer.1 && covers(groups[position], outer.0) && group_rank(groups[position]) == rank;
            assert(listed(ranks, outer.0, outer.1, groups[position]));
        }
        if rank_at(ranks, outer, rank) {
            let (key, position) = choose|key: KeyView, position: int| #![trigger pages.at(key), (*pages.at(key).unwrap().groups)@[position]]
                pages.at(key).is_some() && 0 <= position < (*pages.at(key).unwrap().groups)@.len() && {
                    let group = (*pages.at(key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0) && rank == group_rank(group)
                };
            assert(rank_present((*pages.at(key).unwrap().groups)@, outer, rank));
        }
    }
}

pub fn remove_groups(ranks: &mut Index<Index<()>>, groups: &Vec<GroupCell>)
    requires ranks_wf(*old(ranks)),
        forall|position: int| #![trigger groups@[position]] 0 <= position < groups@.len() ==> groups@[position].bound >= 0,
    ensures ranks_wf(*final(ranks)),
        forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*final(ranks), outer, rank)]
            rank_at(*final(ranks), outer, rank) == (rank_at(*old(ranks), outer, rank) && !rank_present(groups@, outer, rank)),
{
    let ghost original = *ranks;
    let mut position = 0;
    while position < groups.len()
        invariant position <= groups.len(), ranks_wf(*ranks),
            forall|source: int| #![trigger groups@[source]] 0 <= source < groups@.len() ==> groups@[source].bound >= 0,
            forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*ranks, outer, rank)]
                rank_at(*ranks, outer, rank) == (rank_at(original, outer, rank) && !rank_present(groups@.take(position as int), outer, rank)),
        decreases groups.len() - position,
    {
        let ghost previous = *ranks;
        crate::ranks::change(ranks, &groups[position], false);
        proof {
            let prior = groups@.take(position as int);
            let entry = groups@[position as int];
            assert(groups@.take(position as int + 1) =~= prior.push(entry));
            assert forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*ranks, outer, rank)]
                rank_at(*ranks, outer, rank) == (rank_at(original, outer, rank) && !rank_present(groups@.take(position as int + 1), outer, rank)) by {
                assert(rank_present(prior.push(entry), outer, rank) == (rank_present(prior, outer, rank)
                    || ((*entry.segment)@ == outer.1 && covers(entry, outer.0) && group_rank(entry) == rank))) by {
                    if rank_present(prior.push(entry), outer, rank) {
                        let source = choose|source: int| #![trigger prior.push(entry)[source]] 0 <= source < prior.push(entry).len()
                            && (*prior.push(entry)[source].segment)@ == outer.1 && covers(prior.push(entry)[source], outer.0) && group_rank(prior.push(entry)[source]) == rank;
                        if source < prior.len() { assert(rank_present(prior, outer, rank)); }
                    }
                    if (*entry.segment)@ == outer.1 && covers(entry, outer.0) && group_rank(entry) == rank {
                        assert(prior.push(entry)[prior.len() as int] == entry);
                        assert(rank_present(prior.push(entry), outer, rank));
                    }
                    if rank_present(prior, outer, rank) {
                        let source = choose|source: int| #![trigger prior[source]] 0 <= source < prior.len()
                            && (*prior[source].segment)@ == outer.1 && covers(prior[source], outer.0) && group_rank(prior[source]) == rank;
                        assert(prior.push(entry)[source] == prior[source]);
                        assert(rank_present(prior.push(entry), outer, rank));
                    }
                }
                assert(rank_at(previous, outer, rank) == (rank_at(original, outer, rank) && !rank_present(prior, outer, rank)));
            }
        }
        position += 1;
    }
    proof { assert(groups@.take(position as int) =~= groups@); }
}

pub proof fn lemma_other_page(pages: Index<Page>, changed: KeyView, other: KeyView, outer: KeyView, rank: KeyView)
    requires page_rank_shape(pages), changed != other, pages.at(changed).is_some(), pages.at(other).is_some(),
        rank_present((*pages.at(changed).unwrap().groups)@, outer, rank),
    ensures !rank_present((*pages.at(other).unwrap().groups)@, outer, rank),
{
    if rank_present((*pages.at(other).unwrap().groups)@, outer, rank) {
        let left = (*pages.at(changed).unwrap().groups)@;
        let right = (*pages.at(other).unwrap().groups)@;
        let first = choose|first: int| #![trigger left[first]] 0 <= first < left.len()
            && (*left[first].segment)@ == outer.1 && covers(left[first], outer.0) && group_rank(left[first]) == rank;
        let second = choose|second: int| #![trigger right[second]] 0 <= second < right.len()
            && (*right[second].segment)@ == outer.1 && covers(right[second], outer.0) && group_rank(right[second]) == rank;
        lemma_group_identity(left[first].id, right[second].id);
        assert(changed.0 == other.0);
        assert(changed.1 =~= other.1);
    }
}

pub proof fn lemma_page_replacement(before: Index<Page>, after: Index<Page>, key: i64, replacement: Option<Page>, outer: KeyView, rank: KeyView)
    requires page_rank_shape(before),
        forall|target: KeyView| #![trigger after.at(target)]
            after.at(target) == if target == (key as int, Seq::<u8>::empty()) { replacement } else { before.at(target) },
    ensures page_rank(after, outer, rank) == ((page_rank(before, outer, rank)
            && !(before.at((key as int, Seq::empty())).is_some() && rank_present((*before.at((key as int, Seq::empty())).unwrap().groups)@, outer, rank)))
        || (replacement.is_some() && rank_present((*replacement.unwrap().groups)@, outer, rank))),
{
    let changed = (key as int, Seq::<u8>::empty());
    let removed = before.at(changed).is_some() && rank_present((*before.at(changed).unwrap().groups)@, outer, rank);
    if page_rank(after, outer, rank) {
        let source = choose|source: KeyView| #![trigger after.at(source)] after.at(source).is_some() && rank_present((*after.at(source).unwrap().groups)@, outer, rank);
        if source != changed {
            assert(page_rank(before, outer, rank));
            if removed { lemma_other_page(before, changed, source, outer, rank); }
        }
    }
    if page_rank(before, outer, rank) && !removed {
        let source = choose|source: KeyView| #![trigger before.at(source)] before.at(source).is_some() && rank_present((*before.at(source).unwrap().groups)@, outer, rank);
        assert(source != changed);
        assert(after.at(source) == before.at(source));
        assert(page_rank(after, outer, rank));
    }
    if replacement.is_some() && rank_present((*replacement.unwrap().groups)@, outer, rank) {
        assert(after.at(changed) == replacement);
        assert(page_rank(after, outer, rank));
    }
}

pub fn publish_page(pages: &mut Index<Page>, ranks: &mut Index<Index<()>>, key: i64, page: Page)
    requires rank_exact(*old(pages), *old(ranks)), page_rank_shape(*old(pages)),
        forall|position: int| #![trigger (*page.groups)@[position]] 0 <= position < (*page.groups)@.len() ==>
            (*page.groups)@[position].id.key as int / 256 == key as int && (*page.groups)@[position].bound >= 0,
    ensures final(pages).wf(), rank_exact(*final(pages), *final(ranks)), page_rank_shape(*final(pages)),
        forall|target: KeyView| #![trigger final(pages).at(target)]
            final(pages).at(target) == if target == (key as int, Seq::<u8>::empty()) {
                if (*page.orders)@.len() == 0 && (*page.ships)@.len() == 0 && (*page.groups)@.len() == 0 { None } else { Some(page) }
            } else { old(pages).at(target) },
{
    let ghost before = *pages;
    let ghost original_ranks = *ranks;
    let old_page = pages.get(&Key::number(key));
    if let Some(old_page) = old_page { remove_groups(ranks, &old_page.groups); }
    let ghost removed_ranks = *ranks;
    crate::ranks::append_groups(ranks, &page.groups);
    if page.orders.len() == 0 && page.ships.len() == 0 && page.groups.len() == 0 { pages.remove(&Key::number(key)); }
    else { pages.put(Key::number(key), Arc::new(page)); }
    proof {
        let replacement = if (*page.orders)@.len() == 0 && (*page.ships)@.len() == 0 && (*page.groups)@.len() == 0 { None } else { Some(page) };
        assert forall|outer: KeyView, rank: KeyView| #![trigger rank_at(*ranks, outer, rank)]
            rank_at(*ranks, outer, rank) == page_rank(*pages, outer, rank) by {
            lemma_page_replacement(before, *pages, key, replacement, outer, rank);
            assert(rank_at(original_ranks, outer, rank) == page_rank(before, outer, rank));
            if old_page.is_some() {
                assert(rank_at(removed_ranks, outer, rank) == (rank_at(original_ranks, outer, rank)
                    && !rank_present((*(*old_page.unwrap()).groups)@, outer, rank)));
            }
            if replacement.is_none() { assert(!rank_present((*page.groups)@, outer, rank)); }
        }
    }
}

pub proof fn lemma_rank_finish(pages: Index<Page>, ranks: Index<Index<()>>, db: Db)
    requires rank_exact(pages, ranks),
        forall|key: KeyView| #![trigger pages.at(key)] pages.at(key).is_some() ==> key.1.len() == 0 && page_shape(pages.at(key).unwrap(), key.0, db),
    ensures rank_database_ok(pages, ranks),
{
    assert forall|key: KeyView, position: int, node: int| #![trigger listed(ranks, node, (*(*pages.at(key).unwrap().groups)@[position].segment)@, (*pages.at(key).unwrap().groups)@[position])]
        pages.at(key).is_some() && 0 <= position < (*pages.at(key).unwrap().groups)@.len() implies {
            let group = (*pages.at(key).unwrap().groups)@[position];
            listed(ranks, node, (*group.segment)@, group) == covers(group, node)
        } by {
        let group = (*pages.at(key).unwrap().groups)@[position];
        let outer = (node, (*group.segment)@);
        let rank = group_rank(group);
        if covers(group, node) {
            assert(rank_present((*pages.at(key).unwrap().groups)@, outer, rank));
            assert(page_rank(pages, outer, rank));
        }
        if listed(ranks, node, (*group.segment)@, group) {
            assert(rank_at(ranks, outer, rank));
            let source = choose|source: KeyView| #![trigger pages.at(source)] pages.at(source).is_some() && rank_present((*pages.at(source).unwrap().groups)@, outer, rank);
            let rows = (*pages.at(source).unwrap().groups)@;
            let offset = choose|offset: int| #![trigger rows[offset]] 0 <= offset < rows.len()
                && (*rows[offset].segment)@ == outer.1 && covers(rows[offset], outer.0) && group_rank(rows[offset]) == rank;
            crate::semantics::lemma_page_group_unique(pages, db, key, position, source, offset);
        }
    }
    assert forall|outer: KeyView| #![trigger ranks.at(outer)] ranks.at(outer).is_some() implies {
        let inner = ranks.at(outer).unwrap();
        &&& inner.wf()
        &&& forall|rank: KeyView| #![trigger inner.at(rank)] inner.at(rank).is_some() ==>
            exists|key: KeyView, position: int| #![trigger pages.at(key), (*pages.at(key).unwrap().groups)@[position]]
                pages.at(key).is_some() && 0 <= position < (*pages.at(key).unwrap().groups)@.len() && {
                    let group = (*pages.at(key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0) && group_rank(group) == rank
                }
    } by {
        let inner = ranks.at(outer).unwrap();
        assert forall|rank: KeyView| #![trigger inner.at(rank)] inner.at(rank).is_some() implies
            exists|key: KeyView, position: int| #![trigger pages.at(key), (*pages.at(key).unwrap().groups)@[position]]
                pages.at(key).is_some() && 0 <= position < (*pages.at(key).unwrap().groups)@.len() && {
                    let group = (*pages.at(key).unwrap().groups)@[position];
                    (*group.segment)@ == outer.1 && covers(group, outer.0) && group_rank(group) == rank
                } by {
            assert(rank_at(ranks, outer, rank));
            let key = choose|key: KeyView| #![trigger pages.at(key)] pages.at(key).is_some() && rank_present((*pages.at(key).unwrap().groups)@, outer, rank);
            let rows = (*pages.at(key).unwrap().groups)@;
            let position = choose|position: int| #![trigger rows[position]] 0 <= position < rows.len()
                && (*rows[position].segment)@ == outer.1 && covers(rows[position], outer.0) && group_rank(rows[position]) == rank;
            assert((*pages.at(key).unwrap().groups)@[position] == rows[position]);
        }
    }
}

}
