// Build: each dimension of the state from the scans of the database.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::dimensions::*;
use crate::database::*;
use crate::events::*;
use crate::orders::*;
use crate::rollup::*;
use crate::query_proofs::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn pat_views(pats: Seq<Vec<u8>>) -> Seq<Seq<u8>> {
    pat_views_of(pats)
}

pub open spec fn work_mask(work: Work<Vec<MaskRow>>, key: i64, c: int) -> int {
    if work.rows@.contains_key(key) { mask_color(work.rows@[key]@, c) } else { 0int }
}

pub open spec fn mask_in_weight(p: Seq<u8>, key: i64) -> spec_fn(PartIn) -> int {
    |r: PartIn| if r.p_partkey == key && like_match(r.p_name@, p) { 1int } else { 0int }
}

#[verifier::spinoff_prover]
pub fn build_masks(all_parts: &Vec<PartIn>, pats: &Vec<Vec<u8>>, Ghost(db): Ghost<Db>) -> (r: Option<Pages<Vec<MaskRow>>>)
    requires scan_all_parts_ok(db, all_parts@), pats.len() <= 127,
    ensures r matches Some(p) ==> masks_ready(p, db, pat_views(pats@)),
{
    let ghost pv = pat_views(pats@);
    let mut work: Work<Vec<MaskRow>> = Work::new();
    proof {
        assert forall|key: i64, c: int| #[trigger] work_mask(work, key, c) == sum(all_parts@.take(0), mask_in_weight(pv[c], key)) by {
            lemma_sum_empty(all_parts@.take(0), mask_in_weight(pv[c], key));
        }
    }
    let mut index = 0;
    while index < all_parts.len()
        invariant index <= all_parts.len(), work.valid(), pats.len() <= 127, pv == pat_views(pats@),
            forall|key: i64, c: int| 0 <= c < pv.len() ==> #[trigger] work_mask(work, key, c) == sum(all_parts@.take(index as int), mask_in_weight(pv[c], key)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
                let rows = work.rows@[key];
                rows.len() > 0 && masks_unique(rows@) && masks_positive(rows@)
            },
        decreases all_parts.len() - index,
    {
        let row = &all_parts[index];
        let m = mask_of(&row.p_name, pats);
        let ghost before = work;
        let mut rows = match work.take(row.p_partkey) { Some(rows) => rows, None => Vec::new() };
        proof {
            assert(masks_unique(rows@) && masks_positive(rows@));
            assert forall|c: int| 0 <= c < pv.len() implies #[trigger] mask_color(rows@, c) == work_mask(before, row.p_partkey, c) by {
                if !before.rows@.contains_key(row.p_partkey) {
                    lemma_sum_empty(rows@, |r: MaskRow| if bit(r.mask, c) { r.count as int } else { 0int });
                }
            }
        }
        bump_mask(&mut rows, m, 1)?;
        work.put(row.p_partkey, rows);
        proof {
            assert forall|key: i64, c: int| 0 <= c < pv.len() implies #[trigger] work_mask(work, key, c) == sum(all_parts@.take(index as int + 1), mask_in_weight(pv[c], key)) by {
                lemma_sum_take(all_parts@, index as int, mask_in_weight(pv[c], key));
                if key != row.p_partkey { assert(work_mask(work, key, c) == work_mask(before, key, c)); }
                else {
                    assert(bit(m, c) <==> like_match(row.p_name@, pv[c]));
                }
            }
        }
        index += 1;
    }
    proof { assert(all_parts@.take(index as int) =~= all_parts@); }
    let pages = Pages::from_work(work);
    proof {
        assert forall|key: i64, c: int| 0 <= c < pv.len() implies #[trigger] page_mask_color(pages, key, c) == green(filter_db(db, pv[c]), key as int) by {
            let w = matched_weight(pv[c], key as int);
            assert(sum(part_in_seq_view(all_parts@), w) == sum(db.part, w));
            lemma_sum_map(all_parts@, |r: PartIn| part_in_view(r), w, mask_in_weight(pv[c], key));
            lemma_matched_green(db, pv[c], key as int);
            assert(page_mask_color(pages, key, c) == work_mask(work, key, c));
        }
    }
    Some(pages)
}

pub open spec fn work_pairs(work: Work<Vec<Pair>>, key: i64, supplier: int, field: int) -> int {
    if work.rows@.contains_key(key) { pair_at(work.rows@[key]@, supplier, field) } else { 0int }
}

pub open spec fn pair_in_weight(key: i64, supplier: int, field: int) -> spec_fn(PartsuppIn) -> int {
    |r: PartsuppIn| if r.ps_partkey == key && r.ps_suppkey as int == supplier { if field == 0 { 1int } else { r.ps_supplycost as int } } else { 0int }
}

#[verifier::spinoff_prover]
pub fn build_pairs(all_partsupp: &[PartsuppIn], Ghost(db): Ghost<Db>) -> (r: Option<Pages<Vec<Pair>>>)
    requires scan_all_partsupp_ok(db, all_partsupp@),
    ensures r matches Some(p) ==> pairs_ready(p, db),
{
    let mut work: Work<Vec<Pair>> = Work::new();
    proof {
        assert forall|key: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] work_pairs(work, key, s, f) == sum(all_partsupp@.take(0), pair_in_weight(key, s, f)) by {
            lemma_sum_empty(all_partsupp@.take(0), pair_in_weight(key, s, f));
        }
    }
    let mut index = 0;
    while index < all_partsupp.len()
        invariant index <= all_partsupp.len(), work.valid(),
            forall|key: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] work_pairs(work, key, s, f) == sum(all_partsupp@.take(index as int), pair_in_weight(key, s, f)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
                let rows = work.rows@[key];
                rows.len() > 0 && pairs_unique(rows@) && pairs_positive(rows@)
            },
        decreases all_partsupp.len() - index,
    {
        let row = all_partsupp[index];
        let ghost before = work;
        let mut rows = match work.take(row.ps_partkey) { Some(rows) => rows, None => Vec::new() };
        proof {
            assert(pairs_unique(rows@) && pairs_positive(rows@));
            assert forall|s: int, f: int| 0 <= f <= 1 implies #[trigger] pair_at(rows@, s, f) == work_pairs(before, row.ps_partkey, s, f) by {
                if !before.rows@.contains_key(row.ps_partkey) {
                    lemma_sum_empty(rows@, |r: Pair| if r.supplier == s { if f == 0 { r.count as int } else { r.cost as int } } else { 0int });
                }
            }
        }
        bump_pair(&mut rows, row.ps_suppkey, 1, row.ps_supplycost as i128)?;
        work.put(row.ps_partkey, rows);
        proof {
            assert forall|key: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] work_pairs(work, key, s, f) == sum(all_partsupp@.take(index as int + 1), pair_in_weight(key, s, f)) by {
                lemma_sum_take(all_partsupp@, index as int, pair_in_weight(key, s, f));
                if key != row.ps_partkey { assert(work_pairs(work, key, s, f) == work_pairs(before, key, s, f)); }
            }
        }
        index += 1;
    }
    proof { assert(all_partsupp@.take(index as int) =~= all_partsupp@); }
    let pages = Pages::from_work(work);
    proof {
        assert forall|key: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] page_pair(pages, key, s, f)
            == if f == 0 { pair_count(db, key as int, s) } else { pair_cost(db, key as int, s) } by {
            let w = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == s { if f == 0 { 1int } else { row.ps_supplycost } } else { 0int };
            assert(sum(partsupp_in_seq_view(all_partsupp@), w) == sum(db.partsupp, w));
            lemma_sum_map(all_partsupp@, |r: PartsuppIn| partsupp_in_view(r), w, pair_in_weight(key, s, f));
            if f == 0 {
                lemma_sum_ext_all(db.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == s { 1int } else { 0int });
            } else {
                lemma_sum_ext_all(db.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
            }
            assert(page_pair(pages, key, s, f) == work_pairs(work, key, s, f));
        }
    }
    Some(pages)
}

pub open spec fn supplier_in_weight(key: i64, nation: int) -> spec_fn(SupplierIn) -> int {
    |row: SupplierIn| if row.s_suppkey == key && row.s_nationkey as int == nation { 1int } else { 0int }
}

pub open spec fn work_number(work: Work<Vec<Number>>, key: i64, nation: int) -> int {
    if work.rows@.contains_key(key) { number_at(work.rows@[key]@, nation) } else { 0int }
}

#[verifier::spinoff_prover]
pub fn build_suppliers(supplier_nations: &[SupplierIn], Ghost(db): Ghost<Db>) -> (r: Option<Pages<Vec<Number>>>)
    requires scan_supplier_nations_ok(db, supplier_nations@),
    ensures r matches Some(p) ==> supplier_pages_ready(p, db),
{
    let mut work: Work<Vec<Number>> = Work::new();
    proof {
        assert forall|key: i64, nation: int| #[trigger] work_number(work, key, nation) == sum(supplier_nations@.take(0), supplier_in_weight(key, nation)) by {
            lemma_sum_empty(supplier_nations@.take(0), supplier_in_weight(key, nation));
        }
    }
    let mut index = 0;
    while index < supplier_nations.len()
        invariant index <= supplier_nations.len(), work.valid(),
            forall|key: i64, nation: int| #[trigger] work_number(work, key, nation) == sum(supplier_nations@.take(index as int), supplier_in_weight(key, nation)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
                let rows = work.rows@[key];
                rows.len() > 0 && number_unique(rows@) && numbers_positive(rows@)
            },
        decreases supplier_nations.len() - index,
    {
        let row = supplier_nations[index];
        let ghost before = work;
        let mut rows = match work.take(row.s_suppkey) { Some(rows) => rows, None => Vec::new() };
        proof {
            assert(number_unique(rows@) && numbers_positive(rows@));
            assert forall|nation: int| #[trigger] number_at(rows@, nation) == work_number(before, row.s_suppkey, nation) by {
                if !before.rows@.contains_key(row.s_suppkey) {
                    lemma_sum_empty(rows@, |r: Number| if r.key as int == nation { r.count as int } else { 0int });
                }
            }
        }
        bump_number(&mut rows, row.s_nationkey, 1)?;
        work.put(row.s_suppkey, rows);
        proof {
            assert forall|key: i64, nation: int| #[trigger] work_number(work, key, nation) == sum(supplier_nations@.take(index as int + 1), supplier_in_weight(key, nation)) by {
                lemma_sum_take(supplier_nations@, index as int, supplier_in_weight(key, nation));
                if key != row.s_suppkey { assert(work_number(work, key, nation) == work_number(before, key, nation)); }
            }
        }
        index += 1;
    }
    proof { assert(supplier_nations@.take(index as int) =~= supplier_nations@); }
    let pages = Pages::from_work(work);
    proof {
        assert forall|key: i64, nation: int| #[trigger] number_page(pages, key, nation) == supplier_count(db, key as int, nation) by {
            lemma_scan_supplier(db, supplier_nations@, key, nation);
            assert(number_page(pages, key, nation) == work_number(work, key, nation));
        }
    }
    Some(pages)
}

#[verifier::spinoff_prover]
pub fn build_names(nation_names: &Vec<NationIn>, Ghost(db): Ghost<Db>) -> (r: Option<Pages<Vec<Name>>>)
    requires scan_nation_names_ok(db, nation_names@),
    ensures r matches Some(p) ==> nation_pages_ready(p, db),
{
    let mut work: Work<Vec<Name>> = Work::new();
    proof {
        assert forall|key: i64, name: Seq<u8>| #[trigger] work_name(work, key, name) == sum(nation_names@.take(0), nation_input_weight(key, name)) by {
            lemma_sum_empty(nation_names@.take(0), nation_input_weight(key, name));
        }
    }
    let mut index = 0;
    while index < nation_names.len()
        invariant index <= nation_names.len(), work.valid(),
            forall|key: i64, name: Seq<u8>| #[trigger] work_name(work, key, name) == sum(nation_names@.take(index as int), nation_input_weight(key, name)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
                let rows = work.rows@[key];
                rows.len() > 0 && name_unique(rows@) && names_positive(rows@)
            },
        decreases nation_names.len() - index,
    {
        let row = &nation_names[index];
        let ghost before = work;
        let mut rows = match work.take(row.n_nationkey) { Some(rows) => rows, None => Vec::new() };
        proof {
            assert(name_unique(rows@) && names_positive(rows@));
            assert forall|name: Seq<u8>| #[trigger] name_at(rows@, name) == work_name(before, row.n_nationkey, name) by {
                if !before.rows@.contains_key(row.n_nationkey) {
                    lemma_sum_empty(rows@, |r: Name| if r.bytes@ == name { r.count as int } else { 0int });
                }
            }
        }
        let bytes = Arc::new(copy_bytes(&row.n_name));
        bump_name(&mut rows, &bytes, 1)?;
        work.put(row.n_nationkey, rows);
        proof {
            assert forall|key: i64, name: Seq<u8>| #[trigger] work_name(work, key, name) == sum(nation_names@.take(index as int + 1), nation_input_weight(key, name)) by {
                lemma_sum_take(nation_names@, index as int, nation_input_weight(key, name));
                if key != row.n_nationkey { assert(work_name(work, key, name) == work_name(before, key, name)); }
            }
        }
        index += 1;
    }
    proof { assert(nation_names@.take(index as int) =~= nation_names@); }
    let pages = Pages::from_work(work);
    proof {
        assert forall|key: i64, name: Seq<u8>| #[trigger] name_page(pages, key, name) == nation_count(db, key as int, name) by {
            lemma_scan_nation(db, nation_names@, key, name);
            assert(name_page(pages, key, name) == work_name(work, key, name));
        }
    }
    Some(pages)
}

}
