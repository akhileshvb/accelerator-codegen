// Maintenance of the pair rows of the parts: the keys a transaction touches are rebuilt from
// the rescan of partsupp.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::events::*;
use crate::buildparts::*;
use crate::orders::has_key;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn ps_keys(changes: Seq<PartsuppDelta>) -> Seq<int> {
    changes.map_values(|ch: PartsuppDelta| ch.row.ps_partkey as int)
}

/// The rows of a rescan of partsupp grouped by part key.
#[verifier::spinoff_prover]
pub fn pairs_from_rescan(keys: &[i64], rows: &[PartsuppIn]) -> (r: Option<Work<Vec<Pair>>>)
    ensures r matches Some(w) ==> w.valid()
        && (forall|key: i64| #[trigger] keys@.contains(key) ==> w.rows@.contains_key(key))
        && (forall|key: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] work_pairs(w, key, s, f) == sum(rows@, pair_in_weight(key, s, f)))
        && forall|key: i64| #[trigger] w.rows@.contains_key(key) ==> pairs_unique(w.rows@[key]@) && pairs_positive(w.rows@[key]@),
{
    let mut work: Work<Vec<Pair>> = Work::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), work.valid(),
            forall|key: i64| #[trigger] keys@.take(index as int).contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key]@ == Seq::<Pair>::empty(),
        decreases keys.len() - index,
    {
        if !work.rows.contains_key(&keys[index]) { work.put(keys[index], Vec::new()); }
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys[index as int]));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), keys[index as int]);
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|key: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] work_pairs(work, key, s, f) == sum(rows@.take(0), pair_in_weight(key, s, f)) by {
            lemma_sum_empty(rows@.take(0), pair_in_weight(key, s, f));
            if work.rows@.contains_key(key) {
                lemma_sum_empty(work.rows@[key]@, |r: Pair| if r.supplier as int == s { if f == 0 { r.count as int } else { r.cost as int } } else { 0int });
            }
        }
    }
    index = 0;
    while index < rows.len()
        invariant index <= rows.len(), work.valid(),
            forall|key: i64| #[trigger] keys@.contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] work_pairs(work, key, s, f) == sum(rows@.take(index as int), pair_in_weight(key, s, f)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> pairs_unique(work.rows@[key]@) && pairs_positive(work.rows@[key]@),
        decreases rows.len() - index,
    {
        let row = rows[index];
        let ghost before = work;
        let mut pr = match work.take(row.ps_partkey) { Some(r) => r, None => Vec::new() };
        proof {
            assert(pairs_unique(pr@) && pairs_positive(pr@));
            assert forall|s: int, f: int| 0 <= f <= 1 implies #[trigger] pair_at(pr@, s, f) == work_pairs(before, row.ps_partkey, s, f) by {
                if !before.rows@.contains_key(row.ps_partkey) {
                    lemma_sum_empty(pr@, |r: Pair| if r.supplier as int == s { if f == 0 { r.count as int } else { r.cost as int } } else { 0int });
                }
            }
        }
        bump_pair(&mut pr, row.ps_suppkey, 1, row.ps_supplycost as i128)?;
        work.put(row.ps_partkey, pr);
        proof {
            assert forall|key: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] work_pairs(work, key, s, f) == sum(rows@.take(index as int + 1), pair_in_weight(key, s, f)) by {
                lemma_sum_take(rows@, index as int, pair_in_weight(key, s, f));
                if key != row.ps_partkey { assert(work_pairs(work, key, s, f) == work_pairs(before, key, s, f)); }
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(work)
}

pub proof fn lemma_rescan_pairs(db2: Db, keys: Seq<i64>, rows: Seq<PartsuppIn>, key: i64, s: int, f: int)
    requires rescan_partsupp_for_parts_ok(db2, keys, rows), keys.contains(key), 0 <= f <= 1,
    ensures sum(rows, pair_in_weight(key, s, f)) == if f == 0 { pair_count(db2, key as int, s) } else { pair_cost(db2, key as int, s) },
{
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let w = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == s { if f == 0 { 1int } else { row.ps_supplycost } } else { 0int };
    let filtered = |row: PartsuppRow| if seq_view_i64(keys).contains(row.ps_partkey) { w(row) } else { 0int };
    assert(sum(partsupp_in_seq_view(rows), w) == sum(db2.partsupp, filtered));
    lemma_sum_map(rows, |row: PartsuppIn| partsupp_in_view(row), w, pair_in_weight(key, s, f));
    lemma_sum_ext_all(db2.partsupp, filtered, w);
    if f == 0 {
        lemma_sum_ext_all(db2.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == s { 1int } else { 0int });
    } else {
        lemma_sum_ext_all(db2.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
    }
}

pub proof fn lemma_pair_frame(db: Db, db2: Db, changes: Seq<PartsuppDelta>, key: int, s: int, f: int)
    requires bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(changes)), 0 <= f <= 1,
        forall|i: int| 0 <= i < changes.len() ==> (#[trigger] changes[i]).row.ps_partkey != key,
    ensures (if f == 0 { pair_count(db2, key, s) == pair_count(db, key, s) } else { pair_cost(db2, key, s) == pair_cost(db, key, s) }),
{
    let w = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == s { if f == 0 { 1int } else { row.ps_supplycost } } else { 0int };
    let signed = |entry: (PartsuppRow, int)| entry.1 * w(entry.0);
    assert(sum(db2.partsupp, w) == sum(db.partsupp, w) + sum(partsupp_delta_seq_view(changes), signed));
    assert forall|i: int| 0 <= i < partsupp_delta_seq_view(changes).len() implies signed(#[trigger] partsupp_delta_seq_view(changes)[i]) == 0 by {
        assert(changes[i].row.ps_partkey as int != key);
        assert(changes[i].sign as int * 0int == 0) by (nonlinear_arith);
    }
    lemma_sum_zero(partsupp_delta_seq_view(changes), signed);
    if f == 0 {
        lemma_sum_ext_all(db2.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == s { 1int } else { 0int });
        lemma_sum_ext_all(db.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == s { 1int } else { 0int });
    } else {
        lemma_sum_ext_all(db2.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
        lemma_sum_ext_all(db.partsupp, w, |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
    }
}

/// The pair pages after a transaction: the rescanned keys are replaced by their rows.
#[verifier::spinoff_prover]
pub fn publish_pairs(pages: &Pages<Vec<Pair>>, work: &Work<Vec<Pair>>, keys: &[i64], changes: &[PartsuppDelta],
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(rows): Ghost<Seq<PartsuppIn>>) -> (r: Option<Pages<Vec<Pair>>>)
    requires pairs_ready(*pages, db), work.valid(),
        bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(changes@)),
        rescan_partsupp_for_parts_ok(db2, keys@, rows),
        forall|key: i64| #[trigger] keys@.contains(key) ==> work.rows@.contains_key(key),
        forall|key: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] work_pairs(*work, key, s, f) == sum(rows, pair_in_weight(key, s, f)),
        forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> pairs_unique(work.rows@[key]@) && pairs_positive(work.rows@[key]@),
        forall|i: int| 0 <= i < changes@.len() ==> keys@.contains((#[trigger] changes@[i]).row.ps_partkey),
    ensures r matches Some(next) ==> pairs_ready(next, db2),
{
    let mut next = pages.share();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), next.valid(), pages.valid(), pairs_ready(*pages, db), work.valid(),
            forall|key: i64| #[trigger] keys@.contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> pairs_unique(work.rows@[key]@) && pairs_positive(work.rows@[key]@),
            forall|key: i64| #[trigger] next.at(key) == pages.at(key) || keys@.take(index as int).contains(key),
            forall|key: i64| keys@.take(index as int).contains(key) ==> #[trigger] next.at(key).is_some() == (work.rows@[key]@.len() > 0)
                && (next.at(key).is_some() ==> next.at(key).unwrap()@ == work.rows@[key]@),
            forall|key: i64| !keys@.take(index as int).contains(key) ==> #[trigger] next.at(key) == pages.at(key),
        decreases keys.len() - index,
    {
        let key = keys[index];
        proof { assert(keys@.contains(key)); }
        let pr = work.rows.get(&key)?;
        next.put(key, if pr.len() == 0 { None } else { Some(copy_pairs(pr)) });
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), key);
            
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|key: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] page_pair(next, key, s, f)
            == if f == 0 { pair_count(db2, key as int, s) } else { pair_cost(db2, key as int, s) } by {
            if keys@.contains(key) {
                let pr = work.rows@[key];
                if pr@.len() == 0 {
                    lemma_sum_empty(pr@, |r: Pair| if r.supplier as int == s { if f == 0 { r.count as int } else { r.cost as int } } else { 0int });
                }
                assert(page_pair(next, key, s, f) == work_pairs(*work, key, s, f));
                lemma_rescan_pairs(db2, keys@, rows, key, s, f);
            } else {
                assert forall|i: int| 0 <= i < changes@.len() implies (#[trigger] changes@[i]).row.ps_partkey as int != key as int by {
                    assert(keys@.contains(changes@[i].row.ps_partkey));
                }
                lemma_pair_frame(db, db2, changes@, key as int, s, f);
                assert(page_pair(*pages, key, s, f) == if f == 0 { pair_count(db, key as int, s) } else { pair_cost(db, key as int, s) });
                assert(next.at(key) == pages.at(key));
            }
        }
        assert forall|key: i64| #[trigger] next.at(key).is_some() implies {
            let r = next.at(key).unwrap();
            r.len() > 0 && pairs_unique(r@) && pairs_positive(r@)
        } by {}
    }
    Some(next)
}

}
