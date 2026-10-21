use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{KeyView, Index};
use crate::gen_spec::*;
use crate::model::*;
use verus_common::sqlfns::like_match;

verus! {

pub open spec fn materialized(parts: Parts, edges: Edges, blockers: Index<i64>, counts: Counts) -> bool {
    &&& cell_keys_valid(counts)
    &&& forall|key: KeyView| #[trigger] counts.at(key).is_some() ==> counts.at(key).unwrap().count > 0
    &&& forall|key: KeyView| #[trigger] count_at(counts, key)
        == sum(edges.entries(), |row: (KeyView, Index<i64>)| supplier_weight(parts, blockers, row, key))
}

pub proof fn lemma_build_result(db: Db, parts: Parts, edges: Edges, blockers: Index<i64>, counts: Counts, ranks: Ranks)
    requires materialized(parts, edges, blockers, counts), ranks_valid(ranks, counts),
        forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            #[trigger] total(db, brand, kind, size) == sum(edges.entries(),
                |row: (KeyView, Index<i64>)| supplier_weight(parts, blockers, row, group_key(brand, kind, size))),
    ensures counts_valid(counts, db), ranks_valid(ranks, counts),
{
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] count_at(counts, group_key(brand, kind, size)) == total(db, brand, kind, size) by {}
}

pub open spec fn output_model(counts: Counts, params: Params, out: Seq<OutRow>) -> bool {
    &&& forall|index: int| 0 <= index < out.len() ==> {
        let row = #[trigger] out[index];
        row.p_brand != params.p1 && !like_match(row.p_type, params.p2)
            && (row.p_size == params.p3 || row.p_size == params.p4
                || row.p_size == params.p5 || row.p_size == params.p6 || row.p_size == params.p7
                || row.p_size == params.p8 || row.p_size == params.p9 || row.p_size == params.p10)
            && row.supplier_cnt > 0
            && row.supplier_cnt == count_at(counts, group_key(row.p_brand, row.p_type, row.p_size))
    }
    &&& forall|first: int, last: int| 0 <= first < last < out.len() ==>
        !same_group(#[trigger] out[first], #[trigger] out[last]) && !row_lt(out[last], out[first])
    &&& forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] count_at(counts, group_key(brand, kind, size)) > 0 && brand != params.p1
        && !like_match(kind, params.p2)
        && (size == params.p3 || size == params.p4 || size == params.p5 || size == params.p6
            || size == params.p7 || size == params.p8 || size == params.p9 || size == params.p10) ==>
        exists|index: int| 0 <= index < out.len() && (#[trigger] out[index]).p_brand == brand
            && out[index].p_type == kind && out[index].p_size == size
}

pub proof fn lemma_execute_result(db: Db, counts: Counts, params: Params, out: Seq<OutRow>)
    requires counts_valid(counts, db), output_model(counts, params, out),
    ensures query_result(db, params, out),
{
    crate::query_semantics::lemma_query_bridge(db, params);
    assert forall|index: int| 0 <= index < out.len() implies row_ok(db, params, #[trigger] out[index]) by {
        let row = out[index];
        assert(group_count(db, params, row.p_brand, row.p_type, row.p_size) > 0);
    }
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] group_count(db, params, brand, kind, size) > 0 && having(db, params, brand, kind, size)
        implies exists|index: int| 0 <= index < out.len() && (#[trigger] out[index]).p_brand == brand
            && out[index].p_type == kind && out[index].p_size == size by {
        assert(count_at(counts, group_key(brand, kind, size)) > 0);
    }
}

}
