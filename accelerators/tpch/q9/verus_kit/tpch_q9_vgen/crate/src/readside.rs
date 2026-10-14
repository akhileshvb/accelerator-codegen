use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::values::*;
use crate::storage::*;
use crate::dimensions::*;
use crate::output::*;
use crate::aggregate::*;
use crate::query_proofs::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_filtered_green_bound(db: Db, p: Seq<u8>, key: int)
    ensures 0 <= green(filter_db(db, p), key) <= green(db, key),
{
    lemma_matched_green(db, p, key);
    lemma_sum_nonneg(db.part, matched_weight(p, key));
    lemma_sum_le(db.part, matched_weight(p, key), green_weight(key));
}

pub open spec fn emitted(pages: Pages<Vec<Group>>, out: Seq<(Vec<u8>, i64, i128)>) -> bool {
    &&& forall|index: int| 0 <= index < out.len() ==> {
        let row = #[trigger] out_view(out)[index];
        page_group(pages, row.nation, row.o_year, 0) > 0 && row.sum_profit == page_group(pages, row.nation, row.o_year, 1)
    }
    &&& forall|left: int, right: int| 0 <= left < right < out.len() ==>
        !same_group(#[trigger] out_view(out)[left], #[trigger] out_view(out)[right]) && !row_lt(out_view(out)[right], out_view(out)[left])
    &&& forall|name: Seq<u8>, year: int| #[trigger] page_group(pages, name, year, 0) > 0 ==>
        exists|index: int| 0 <= index < out.len() && (#[trigger] out_view(out)[index]).nation == name && out_view(out)[index].o_year == year
}

/// The rows of a table of groups, sorted by ORDER BY.
#[verifier::spinoff_prover]
pub fn emit(pages: &Pages<Vec<Group>>) -> (r: Option<Vec<(Vec<u8>, i64, i128)>>)
    requires groups_shape(*pages),
    ensures r matches Some(out) ==> emitted(*pages, out@),
{
    let keys = pages.keys();
    proof { lemma_cache_meaning(*pages, keys@); }
    let mut output = Vec::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            groups_shape(*pages),
            keys@.no_duplicates(),
            forall|key: i64| #[trigger] pages.at(key).is_some() <==> keys@.contains(key),
            out_view(output@) == cache_rows(*pages, keys@.take(index as int)),
        decreases keys.len() - index,
    {
        let groups = pages.get(keys[index])?;
        let ghost prefix = out_view(output@);
        let mut group_index = 0;
        while group_index < groups.len()
            invariant group_index <= groups.len(),
                groups_shape(*pages),
                index < keys.len(), pages.at(keys@[index as int]) == Some(*groups),
                prefix == cache_rows(*pages, keys@.take(index as int)),
                out_view(output@) == prefix + bucket_rows(groups@.take(group_index as int)),
            decreases groups.len() - group_index,
        {
            let group = &groups[group_index];
            let mut total_index = 0;
            while total_index < group.totals.len()
                invariant total_index <= group.totals.len(),
                    groups_shape(*pages),
                    group_index < groups.len(), *group == groups@[group_index as int],
                    out_view(output@) == prefix + bucket_rows(groups@.take(group_index as int)) + group_rows(*group).take(total_index as int),
                decreases group.totals.len() - total_index,
            {
                let total = group.totals[total_index];
                if total.count <= 0 { return None; }
                let ghost previous = out_view(output@);
                output.push((copy_bytes(&group.name), total.year, total.profit));
                proof {
                    let row = group_rows(*group)[total_index as int];
                    assert(out_view(output@) =~= previous.push(row));
                    assert(group_rows(*group).take(total_index as int + 1) =~= group_rows(*group).take(total_index as int).push(row));
                    assert(out_view(output@) =~= prefix + bucket_rows(groups@.take(group_index as int)) + group_rows(*group).take(total_index as int + 1));
                }
                total_index += 1;
            }
            proof {
                assert(group_rows(*group).take(total_index as int) =~= group_rows(*group));
                assert(groups@.take(group_index as int + 1) =~= groups@.take(group_index as int).push(*group));
                verus_prooflib::lemma_flat_map_push(groups@.take(group_index as int), *group, |group: Group| group_rows(group));
            }
            group_index += 1;
        }
        proof {
            assert(groups@.take(group_index as int) =~= groups@);
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys@[index as int]));
            verus_prooflib::lemma_flat_map_push(keys@.take(index as int), keys@[index as int], |key: i64| cache_page(*pages, key));
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        lemma_cache_meaning(*pages, keys@);
    }
    let ghost before = out_view(output@);
    let result = sort_rows(output);
    proof {
        assert forall|index: int| 0 <= index < result.len() implies {
            let row = #[trigger] out_view(result@)[index];
            page_group(*pages, row.nation, row.o_year, 0) > 0 && row.sum_profit == page_group(*pages, row.nation, row.o_year, 1)
        } by {
            let row = out_view(result@)[index];
            assert(out_view(result@).contains(row));
            assert(before.contains(row));
            let position = choose|position: int| 0 <= position < before.len() && (#[trigger] before[position]) == row;
        }
        assert forall|name: Seq<u8>, year: int| #[trigger] page_group(*pages, name, year, 0) > 0 implies
            exists|index: int| 0 <= index < result.len() && (#[trigger] out_view(result@)[index]).nation == name && out_view(result@)[index].o_year == year by {
            let row = choose|row: OutRow| #[trigger] cache_rows(*pages, keys@).contains(row) && row.nation == name && row.o_year == year;
            assert(out_view(result@).contains(row));
            let position = choose|position: int| 0 <= position < result.len() && (#[trigger] out_view(result@)[position]) == row;
        }
    }
    Some(result)
}

/// The rows emitted from the groups of the filtered database answer the query for the pattern.
pub proof fn lemma_read_meaning(db: Db, p: Seq<u8>, pages: Pages<Vec<Group>>, out: Seq<(Vec<u8>, i64, i128)>)
    requires
        forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] page_group(pages, name, year, field) == group_database(filter_db(db, p), name, year, field),
        emitted(pages, out),
    ensures query_result(db, params(p), out_view(out)),
{
    let pp = params(p);
    assert forall|index: int| 0 <= index < out_view(out).len() implies row_ok(db, pp, #[trigger] out_view(out)[index]) by {
        let row = out_view(out)[index];
        lemma_query_groups(db, pp, row.nation, row.o_year, 0);
        lemma_query_groups(db, pp, row.nation, row.o_year, 1);
        assert(page_group(pages, row.nation, row.o_year, 0) == group_count(db, pp, row.nation, row.o_year));
        assert(page_group(pages, row.nation, row.o_year, 1) == group_sum(db, pp, row.nation, row.o_year, |tuple: Tuple| tuple.profit.amount));
    }
    assert forall|name: Seq<u8>, year: int| #[trigger] group_count(db, pp, name, year) > 0 && having(db, pp, name, year)
        implies exists|index: int| 0 <= index < out_view(out).len() && (#[trigger] out_view(out)[index]).nation == name && out_view(out)[index].o_year == year by {
        lemma_query_groups(db, pp, name, year, 0);
        assert(page_group(pages, name, year, 0) == group_count(db, pp, name, year));
    }
}

}
