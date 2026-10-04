use vstd::prelude::*;
use verus_common::bag::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::meaning::*;
use crate::aggregate::{prune_numbers, prune_names};
use std::sync::Arc;

verus! {

pub open spec fn number_page(pages: Pages<Vec<Number>>, key: i64, nation: int) -> int {
    match pages.at(key) { Some(rows) => number_at(rows@, nation), None => 0 }
}

pub open spec fn name_page(pages: Pages<Vec<Name>>, key: i64, name: Seq<u8>) -> int {
    match pages.at(key) { Some(rows) => name_at(rows@, name), None => 0 }
}

pub open spec fn number_overlay(pages: Pages<Vec<Number>>, work: Work<Vec<Number>>, key: i64, nation: int) -> int {
    if work.rows@.contains_key(key) { number_at(work.rows@[key]@, nation) } else { number_page(pages, key, nation) }
}

pub open spec fn name_overlay(pages: Pages<Vec<Name>>, work: Work<Vec<Name>>, key: i64, name: Seq<u8>) -> int {
    if work.rows@.contains_key(key) { name_at(work.rows@[key]@, name) } else { name_page(pages, key, name) }
}

pub open spec fn number_unique(rows: Seq<Number>) -> bool {
    rows.map_values(|row: Number| row.key).no_duplicates()
}

pub open spec fn name_unique(rows: Seq<Name>) -> bool {
    rows.map_values(|row: Name| row.bytes@).no_duplicates()
}

pub open spec fn supplier_pages_ready(pages: Pages<Vec<Number>>, db: Db) -> bool {
    pages.valid()
        && (forall|key: i64, nation: int| #[trigger] number_page(pages, key, nation) == supplier_count(db, key as int, nation))
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> {
            let rows = pages.at(key).unwrap();
            rows.len() > 0 && number_unique(rows@) && numbers_positive(rows@)
        }
}

pub open spec fn nation_pages_ready(pages: Pages<Vec<Name>>, db: Db) -> bool {
    pages.valid()
        && (forall|key: i64, name: Seq<u8>| #[trigger] name_page(pages, key, name) == nation_count(db, key as int, name))
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> {
            let rows = pages.at(key).unwrap();
            rows.len() > 0 && name_unique(rows@) && names_positive(rows@)
        }
}

pub open spec fn supplier_overlay_ready(pages: Pages<Vec<Number>>, work: Work<Vec<Number>>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64, nation: int| #[trigger] number_overlay(pages, work, key, nation) == supplier_count(db, key as int, nation))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let rows = work.rows@[key];
            number_unique(rows@) && numbers_positive(rows@)
        }
}

pub open spec fn nation_overlay_ready(pages: Pages<Vec<Name>>, work: Work<Vec<Name>>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64, name: Seq<u8>| #[trigger] name_overlay(pages, work, key, name) == nation_count(db, key as int, name))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let rows = work.rows@[key];
            name_unique(rows@) && names_positive(rows@)
        }
}

pub open spec fn supplier_change_weight(key: i64, nation: int) -> spec_fn(SupplierDelta) -> int {
    |change: SupplierDelta| if change.row.s_suppkey == key && change.row.s_nationkey as int == nation { change.sign as int } else { 0int }
}

pub open spec fn nation_change_weight(key: i64, name: Seq<u8>) -> spec_fn(NationDelta) -> int {
    |change: NationDelta| if change.row.n_nationkey == key && change.row.n_name@ == name { change.sign as int } else { 0int }
}

pub proof fn lemma_supplier_capture(db: Db, db2: Db, changes: Seq<SupplierDelta>, key: i64, nation: int)
    requires bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(changes)),
    ensures supplier_count(db2, key as int, nation) == supplier_count(db, key as int, nation) + sum(changes, supplier_change_weight(key, nation)),
{
    let weight = |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation { 1int } else { 0int };
    let signed = |entry: (SupplierRow, int)| entry.1 * weight(entry.0);
    assert(sum(db2.supplier, weight) == sum(db.supplier, weight) + sum(supplier_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |change: SupplierDelta| (supplier_in_view(change.row), change.sign as int), signed, supplier_change_weight(key, nation));
}

pub proof fn lemma_nation_capture(db: Db, db2: Db, changes: Seq<NationDelta>, key: i64, name: Seq<u8>)
    requires bag_step(db.nation, db2.nation, nation_delta_seq_view(changes)),
    ensures nation_count(db2, key as int, name) == nation_count(db, key as int, name) + sum(changes, nation_change_weight(key, name)),
{
    let weight = |row: NationRow| if row.n_nationkey == key as int && row.n_name == name { 1int } else { 0int };
    let signed = |entry: (NationRow, int)| entry.1 * weight(entry.0);
    assert(sum(db2.nation, weight) == sum(db.nation, weight) + sum(nation_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |change: NationDelta| (nation_in_view(change.row), change.sign as int), signed, nation_change_weight(key, name));
}

#[verifier::spinoff_prover]
pub fn update_suppliers(pages: &Pages<Vec<Number>>, changes: &[SupplierDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (result: Option<Work<Vec<Number>>>)
    requires supplier_pages_ready(*pages, db), bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(changes@)),
    ensures result matches Some(work) ==> supplier_overlay_ready(*pages, work, db2),
{
    let mut work: Work<Vec<Number>> = Work::new();
    proof {
        assert forall|key: i64, nation: int| #[trigger] number_overlay(*pages, work, key, nation)
            == number_page(*pages, key, nation) + sum(changes@.take(0), supplier_change_weight(key, nation)) by {
            lemma_sum_empty(changes@.take(0), supplier_change_weight(key, nation));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int,
            supplier_pages_ready(*pages, db), work.valid(),
            forall|key: i64, nation: int| #[trigger] number_overlay(*pages, work, key, nation)
                == number_page(*pages, key, nation) + sum(changes@.take(processed@), supplier_change_weight(key, nation)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> number_unique(work.rows@[key]@),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let key = change.row.s_suppkey;
        let ghost before = work;
        let mut rows = match work.take(key) {
            Some(rows) => rows,
            None => match pages.get(key) { Some(rows) => copy_numbers(&rows), None => Vec::new() },
        };
        proof {
            assert(number_unique(rows@));
            assert forall|nation: int| #[trigger] number_at(rows@, nation) == number_overlay(*pages, before, key, nation) by {
                if !before.rows@.contains_key(key) && pages.at(key).is_none() {
                    lemma_sum_empty(rows@, |row: Number| if row.key == nation { row.count as int } else { 0int });
                }
            }
        }
        bump_number(&mut rows, change.row.s_nationkey, change.sign as i128)?;
        work.put(key, rows);
        proof {
            assert forall|target: i64, nation: int| #[trigger] number_overlay(*pages, work, target, nation)
                == number_page(*pages, target, nation) + sum(changes@.take(index as int + 1), supplier_change_weight(target, nation)) by {
                lemma_sum_take(changes@, index as int, supplier_change_weight(target, nation));
                if target != key { assert(number_overlay(*pages, work, target, nation) == number_overlay(*pages, before, target, nation)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(changes@.take(processed@) =~= changes@);
        assert forall|key: i64, nation: int| #[trigger] number_overlay(*pages, work, key, nation) == supplier_count(db2, key as int, nation) by {
            lemma_supplier_capture(db, db2, changes@, key, nation);
        }
    }
    index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), supplier_pages_ready(*pages, db),
            forall|key: i64, nation: int| #[trigger] number_overlay(*pages, work, key, nation) == supplier_count(db2, key as int, nation),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> number_unique(work.rows@[key]@),
            forall|key: i64| work.keys@.take(index as int).contains(key) && #[trigger] work.rows@.contains_key(key) ==> numbers_positive(work.rows@[key]@),
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        let ghost before = work;
        let mut rows = work.take(key)?;
        prune_numbers(&mut rows)?;
        work.put(key, rows);
        proof {
            assert forall|target: i64, nation: int| #[trigger] number_overlay(*pages, work, target, nation) == supplier_count(db2, target as int, nation) by {
                assert(number_overlay(*pages, work, target, nation) == number_overlay(*pages, before, target, nation));
            }
        }
        index += 1;
    }
    proof {
        assert(work.keys@.take(index as int) =~= work.keys@);
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies numbers_positive(work.rows@[key]@) by {
            assert(work.keys@.contains(key));
        }
    }
    Some(work)
}

#[verifier::spinoff_prover]
pub fn update_names(pages: &Pages<Vec<Name>>, changes: &Vec<NationDelta>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (result: Option<Work<Vec<Name>>>)
    requires nation_pages_ready(*pages, db), bag_step(db.nation, db2.nation, nation_delta_seq_view(changes@)),
    ensures result matches Some(work) ==> nation_overlay_ready(*pages, work, db2),
{
    let mut work: Work<Vec<Name>> = Work::new();
    proof {
        assert forall|key: i64, name: Seq<u8>| #[trigger] name_overlay(*pages, work, key, name)
            == name_page(*pages, key, name) + sum(changes@.take(0), nation_change_weight(key, name)) by {
            lemma_sum_empty(changes@.take(0), nation_change_weight(key, name));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int,
            nation_pages_ready(*pages, db), work.valid(),
            forall|key: i64, name: Seq<u8>| #[trigger] name_overlay(*pages, work, key, name)
                == name_page(*pages, key, name) + sum(changes@.take(processed@), nation_change_weight(key, name)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> name_unique(work.rows@[key]@),
        decreases changes.len() - index,
    {
        let change = &changes[index];
        let key = change.row.n_nationkey;
        let ghost before = work;
        let mut rows = match work.take(key) {
            Some(rows) => rows,
            None => match pages.get(key) { Some(rows) => copy_names(&rows), None => Vec::new() },
        };
        proof {
            assert(name_unique(rows@));
            assert forall|name: Seq<u8>| #[trigger] name_at(rows@, name) == name_overlay(*pages, before, key, name) by {
                if !before.rows@.contains_key(key) && pages.at(key).is_none() {
                    lemma_sum_empty(rows@, |row: Name| if row.bytes@ == name { row.count as int } else { 0int });
                }
            }
        }
        let bytes = Arc::new(copy_bytes(&change.row.n_name));
        bump_name(&mut rows, &bytes, change.sign as i128)?;
        work.put(key, rows);
        proof {
            assert forall|target: i64, name: Seq<u8>| #[trigger] name_overlay(*pages, work, target, name)
                == name_page(*pages, target, name) + sum(changes@.take(index as int + 1), nation_change_weight(target, name)) by {
                lemma_sum_take(changes@, index as int, nation_change_weight(target, name));
                if target != key { assert(name_overlay(*pages, work, target, name) == name_overlay(*pages, before, target, name)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(changes@.take(processed@) =~= changes@);
        assert forall|key: i64, name: Seq<u8>| #[trigger] name_overlay(*pages, work, key, name) == nation_count(db2, key as int, name) by {
            lemma_nation_capture(db, db2, changes@, key, name);
        }
    }
    index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), nation_pages_ready(*pages, db),
            forall|key: i64, name: Seq<u8>| #[trigger] name_overlay(*pages, work, key, name) == nation_count(db2, key as int, name),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> name_unique(work.rows@[key]@),
            forall|key: i64| work.keys@.take(index as int).contains(key) && #[trigger] work.rows@.contains_key(key) ==> names_positive(work.rows@[key]@),
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        let ghost before = work;
        let mut rows = work.take(key)?;
        prune_names(&mut rows)?;
        work.put(key, rows);
        proof {
            assert forall|target: i64, name: Seq<u8>| #[trigger] name_overlay(*pages, work, target, name) == nation_count(db2, target as int, name) by {
                assert(name_overlay(*pages, work, target, name) == name_overlay(*pages, before, target, name));
            }
        }
        index += 1;
    }
    proof {
        assert(work.keys@.take(index as int) =~= work.keys@);
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies names_positive(work.rows@[key]@) by {
            assert(work.keys@.contains(key));
        }
    }
    Some(work)
}

#[verifier::spinoff_prover]
pub fn publish_suppliers(pages: &Pages<Vec<Number>>, work: &mut Work<Vec<Number>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (result: Option<Pages<Vec<Number>>>)
    requires supplier_pages_ready(*pages, db), supplier_overlay_ready(*pages, *old(work), db2),
    ensures final(work).valid(), result matches Some(next) ==> supplier_pages_ready(next, db2),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = *work;
    let mut next = pages.share();
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), next.valid(),
            work.keys@ == initial.keys@, initial.valid(),
            supplier_pages_ready(*pages, db), supplier_overlay_ready(*pages, initial, db2),
            forall|key: i64| #[trigger] work.rows@.contains_key(key)
                <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] next.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                let rows = initial.rows@[key];
                if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let rows = work.take(key)?;
        next.put(key, if rows.len() == 0 { None } else { Some(rows) });
        proof {
            assert forall|target: i64| #[trigger] next.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                let rows = initial.rows@[target];
                if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(target) } by {
                assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            }
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target)
                <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {
                assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            }
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64, nation: int| #[trigger] number_page(next, key, nation) == supplier_count(db2, key as int, nation) by {
            if initial.rows@.contains_key(key) {
                assert(initial.keys@.contains(key));
                let rows = initial.rows@[key];
                if rows.len() == 0 {
                    lemma_sum_empty(rows@, |row: Number| if row.key == nation { row.count as int } else { 0int });
                }
            }
            assert(number_page(next, key, nation) == number_overlay(*pages, initial, key, nation));
        }
        assert forall|key: i64| #[trigger] next.at(key).is_some() implies {
            let rows = next.at(key).unwrap();
            rows.len() > 0 && number_unique(rows@) && numbers_positive(rows@)
        } by {
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
        }
    }
    Some(next)
}

#[verifier::spinoff_prover]
pub fn publish_names(pages: &Pages<Vec<Name>>, work: &mut Work<Vec<Name>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (result: Option<Pages<Vec<Name>>>)
    requires nation_pages_ready(*pages, db), nation_overlay_ready(*pages, *old(work), db2),
    ensures final(work).valid(), result matches Some(next) ==> nation_pages_ready(next, db2),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = *work;
    let mut next = pages.share();
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), next.valid(),
            work.keys@ == initial.keys@, initial.valid(),
            nation_pages_ready(*pages, db), nation_overlay_ready(*pages, initial, db2),
            forall|key: i64| #[trigger] work.rows@.contains_key(key)
                <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] next.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                let rows = initial.rows@[key];
                if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let rows = work.take(key)?;
        next.put(key, if rows.len() == 0 { None } else { Some(rows) });
        proof {
            assert forall|target: i64| #[trigger] next.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                let rows = initial.rows@[target];
                if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(target) } by {
                assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            }
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target)
                <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {
                assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            }
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64, name: Seq<u8>| #[trigger] name_page(next, key, name) == nation_count(db2, key as int, name) by {
            if initial.rows@.contains_key(key) {
                assert(initial.keys@.contains(key));
                let rows = initial.rows@[key];
                if rows.len() == 0 {
                    lemma_sum_empty(rows@, |row: Name| if row.bytes@ == name { row.count as int } else { 0int });
                }
            }
            assert(name_page(next, key, name) == name_overlay(*pages, initial, key, name));
        }
        assert forall|key: i64| #[trigger] next.at(key).is_some() implies {
            let rows = next.at(key).unwrap();
            rows.len() > 0 && name_unique(rows@) && names_positive(rows@)
        } by {
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
        }
    }
    Some(next)
}

}
