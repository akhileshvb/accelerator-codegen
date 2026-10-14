// Execution helpers: the rollup of one tracked pattern as the answer.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::dimensions::*;
use crate::aggregate::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub fn bytes_eq_slice(a: &[u8], b: &[u8]) -> (r: bool)
    ensures r == (a@ == b@),
{
    if a.len() != b.len() { return false; }
    let mut i = 0;
    while i < a.len()
        invariant i <= a.len(), a.len() == b.len(), forall|j: int| 0 <= j < i ==> a@[j] == b@[j],
        decreases a.len() - i,
    {
        if a[i] != b[i] { return false; }
        i += 1;
    }
    proof { assert(a@ =~= b@); }
    true
}

/// The nation totals of a rollup as a work map.
#[verifier::spinoff_prover]
pub fn cells_to_totals(cells: &Vec<RCell>) -> (r: Option<Work<Vec<Total>>>)
    ensures r matches Some(w) ==> totals_work_valid(w)
        && forall|key: i64, year: int, field: int| 0 <= field <= 1 ==> #[trigger] work_total(w, key, year, field) == cell_at(cells@, key as int, year, field),
{
    let mut work: Work<Vec<Total>> = Work::new();
    proof {
        assert forall|key: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(work, key, year, field) == cell_at(cells@.take(0), key as int, year, field) by {
            lemma_sum_empty(cells@.take(0), |row: RCell| if row.nk == key as int && row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
        }
    }
    let mut index = 0;
    while index < cells.len()
        invariant index <= cells.len(), totals_work_valid(work),
            forall|key: i64, year: int, field: int| 0 <= field <= 1 ==> #[trigger] work_total(work, key, year, field) == cell_at(cells@.take(index as int), key as int, year, field),
        decreases cells.len() - index,
    {
        let cell = cells[index];
        if cell.count < 0 { return None; }
        let ghost before = work;
        if cell.count == 0 {
            if cell.profit != 0 { return None; }
        } else {
            add_work_total(&mut work, cell.nk, cell.year, cell.count, cell.profit)?;
        }
        proof {
            assert forall|key: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(work, key, year, field) == cell_at(cells@.take(index as int + 1), key as int, year, field) by {
                lemma_sum_take(cells@, index as int, |row: RCell| if row.nk == key as int && row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
            }
        }
        index += 1;
    }
    proof { assert(cells@.take(index as int) =~= cells@); }
    Some(work)
}

/// The nation-name dimension as a work map.
#[verifier::spinoff_prover]
pub fn names_to_work(pages: &Pages<Vec<Name>>, Ghost(db): Ghost<Db>) -> (r: Option<Work<Vec<Name>>>)
    requires nation_pages_ready(*pages, db),
    ensures r matches Some(w) ==> names_ready(w, db),
{
    let keys = pages.keys();
    let mut work: Work<Vec<Name>> = Work::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), pages.valid(), nation_pages_ready(*pages, db),
            keys@.no_duplicates(),
            forall|key: i64| #[trigger] pages.at(key).is_some() <==> keys@.contains(key),
            work.valid(),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> pages.at(key).is_some() && work.rows@[key]@ == pages.at(key).unwrap()@,
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost before = work;
        let rows = pages.get(key)?;
        work.put(key, copy_names(&rows));
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            assert forall|k: i64| #[trigger] work.rows@.contains_key(k) <==> keys@.take(index as int + 1).contains(k) by {
                assert(keys@.take(index as int + 1).contains(k) <==> keys@.take(index as int).contains(k) || k == key);
            }
            assert forall|k: i64| #[trigger] work.rows@.contains_key(k) implies pages.at(k).is_some() && work.rows@[k]@ == pages.at(k).unwrap()@ by {
                if k != key { assert(before.rows@.contains_key(k)); } else { assert(pages.at(key) == Some(*rows)); }
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|key: i64, name: Seq<u8>| #[trigger] work_name(work, key, name) == nation_count(db, key as int, name) by {
            assert(name_page(*pages, key, name) == nation_count(db, key as int, name));
            if work.rows@.contains_key(key) {} else { assert(pages.at(key).is_none()); }
        }
    }
    Some(work)
}

}
