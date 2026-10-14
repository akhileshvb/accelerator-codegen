use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::lemma_count_weight_add;
use crate::aggregation::*;
use crate::changes::*;
use crate::maintenance::{Change, read_moment, adjust_group};
use crate::model::*;
use crate::storage::*;
use crate::variants::*;

verus! {

#[verifier::rlimit(1)]
pub fn apply_key_patch(parts: &mut Store<Vec<Variant>>, lines: &Store<Moment>, groups: &mut Store<Vec<Group>>, key: i64, patch: &Vec<Change>) -> (result: bool)
    requires part_store_wf(*old(parts)), lines.wf(), group_store_wf(*old(groups)), change_bucket_wf(patch@),
    ensures part_store_wf(*final(parts)), group_store_wf(*final(groups)),
        result ==> forall|other: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(*final(parts), other, brand, container)
            == multiplicity(*old(parts), other, brand, container) + if other == key { change_total(patch@, brand, container) } else { 0 },
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*final(groups), brand, container)
            == group_count_at(*old(groups), brand, container) + change_total(patch@, brand, container) * moment_at(*lines, key).count as int,
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*final(groups), brand, container)
            == group_price_at(*old(groups), brand, container) + change_total(patch@, brand, container) * moment_at(*lines, key).price as int,
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*final(groups), brand, container)
            == group_refs_at(*old(groups), brand, container) + change_total(patch@, brand, container),
{
    let before_parts = Ghost(*parts);
    let before_groups = Ghost(*groups);
    let mut variants: Vec<Variant> = Vec::new();
    if let Some(previous) = parts.get(key) {
        let mut index = 0;
        while index < previous.len()
            invariant index <= previous.len(), variants@ == previous@.take(index as int),
                *parts == before_parts@, *groups == before_groups@,
                part_store_wf(*parts), group_store_wf(*groups), lines.wf(),
                previous@ == variants_at(*parts, key),
            decreases previous.len() - index,
        {
            variants.push(Variant { identity: previous[index].identity.clone(), multiplicity: previous[index].multiplicity });
            index += 1;
        }
    }
    let before_variants = Ghost(variants@);
    proof {
        assert(variants@ == variants_at(*parts, key));
        assert(variants_wf(variants@));
        assert(patch@.take(0) =~= Seq::<Change>::empty());
        reveal(change_total);
        reveal(multiplicity);
    }
    let line = read_moment(lines, key);
    let mut index = 0;
    while index < patch.len()
        invariant index <= patch.len(), *parts == before_parts@, part_store_wf(*parts), lines.wf(),
            group_store_wf(*groups), variants_wf(variants@), change_bucket_wf(patch@),
            line == moment_at(*lines, key), before_variants@ == variants_at(before_parts@, key),
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] variant_total(variants@, brand, container)
                == variant_total(before_variants@, brand, container) + change_total(patch@.take(index as int), brand, container),
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*groups, brand, container)
                == group_count_at(before_groups@, brand, container) + change_total(patch@.take(index as int), brand, container) * line.count as int,
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*groups, brand, container)
                == group_price_at(before_groups@, brand, container) + change_total(patch@.take(index as int), brand, container) * line.price as int,
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*groups, brand, container)
                == group_refs_at(before_groups@, brand, container) + change_total(patch@.take(index as int), brand, container),
        decreases patch.len() - index,
    {
        let change = &patch[index];
        let old_variants = Ghost(variants@);
        let old_groups = Ghost(*groups);
        if change.delta != 0 {
            proof { lemma_change_entry(patch@, index as int); }
            if !update_variant(&mut variants, change.identity.clone(), change.delta) { return false; }
            let count = match change.delta.checked_mul(line.count) { Some(value) => value, None => return false };
            let price = match change.delta.checked_mul(line.price) { Some(value) => value, None => return false };
            if !adjust_group(groups, &change.identity, change.delta, count, price) { return false; }
        }
        proof {
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] variant_total(variants@, brand, container)
                == variant_total(before_variants@, brand, container) + change_total(patch@.take(index as int + 1), brand, container) by {
                assert(variant_total(old_variants@, brand, container) == variant_total(before_variants@, brand, container) + change_total(patch@.take(index as int), brand, container));
                lemma_change_prefix(patch@, index as int, brand, container);
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*groups, brand, container)
                == group_count_at(before_groups@, brand, container) + change_total(patch@.take(index as int + 1), brand, container) * line.count as int by {
                assert(group_count_at(old_groups@, brand, container) == group_count_at(before_groups@, brand, container) + change_total(patch@.take(index as int), brand, container) * line.count as int);
                lemma_change_prefix(patch@, index as int, brand, container);
                lemma_count_weight_add(change_total(patch@.take(index as int), brand, container),
                    if same(*change.identity, brand, container) { change.delta as int } else { 0int }, line.count as int);
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*groups, brand, container)
                == group_price_at(before_groups@, brand, container) + change_total(patch@.take(index as int + 1), brand, container) * line.price as int by {
                assert(group_price_at(old_groups@, brand, container) == group_price_at(before_groups@, brand, container) + change_total(patch@.take(index as int), brand, container) * line.price as int);
                lemma_change_prefix(patch@, index as int, brand, container);
                lemma_count_weight_add(change_total(patch@.take(index as int), brand, container),
                    if same(*change.identity, brand, container) { change.delta as int } else { 0int }, line.price as int);
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*groups, brand, container)
                == group_refs_at(before_groups@, brand, container) + change_total(patch@.take(index as int + 1), brand, container) by {
                assert(group_refs_at(old_groups@, brand, container) == group_refs_at(before_groups@, brand, container) + change_total(patch@.take(index as int), brand, container));
                lemma_change_prefix(patch@, index as int, brand, container);
            }
        }
        index += 1;
    }
    proof { assert(patch@.take(patch@.len() as int) =~= patch@); }
    let after_variants = Ghost(variants@);
    if variants.len() == 0 { parts.put(key, None); } else { parts.put(key, Some(Arc::new(variants))); }
    proof {
        assert forall|other: i64| #[trigger] variants_at(*parts, other)
            == if other == key { after_variants@ } else { variants_at(before_parts@, other) } by {}
        lemma_part_publication(before_parts@, *parts, key, after_variants@);
        assert forall|other: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(*parts, other, brand, container)
            == multiplicity(before_parts@, other, brand, container) + if other == key { change_total(patch@, brand, container) } else { 0 } by {
            reveal(multiplicity);
        }
    }
    true
}

}
