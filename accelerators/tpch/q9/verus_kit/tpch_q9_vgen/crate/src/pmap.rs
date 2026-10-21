// Exact keyed accumulators held persistently: a trie of small vectors of cells
// (k2, k3) -> (a, b, c), updated through an overlay of the touched buckets and then published.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::storage::*;
use crate::wide::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Cell { pub k2: i64, pub k3: i64, pub a: Wide, pub b: Wide, pub c: Wide }

pub open spec fn cell_w(k2: int, k3: int, f: int) -> spec_fn(Cell) -> int {
    |e: Cell| if e.k2 == k2 && e.k3 == k3 { if f == 0 { e.a@ } else if f == 1 { e.b@ } else { e.c@ } } else { 0int }
}

pub open spec fn cell_at(rows: Seq<Cell>, k2: int, k3: int, f: int) -> int {
    sum(rows, cell_w(k2, k3, f))
}

pub open spec fn cell_keys(rows: Seq<Cell>) -> Seq<(i64, i64)> {
    rows.map_values(|e: Cell| (e.k2, e.k3))
}

pub open spec fn d3(da: int, db: int, dc: int, f: int) -> int {
    if f == 0 { da } else if f == 1 { db } else { dc }
}

pub fn copy_cells(rows: &Vec<Cell>) -> (r: Vec<Cell>)
    ensures r@.len() == rows@.len(), forall|k2: int, k3: int, f: int| #[trigger] cell_at(r@, k2, k3, f) == cell_at(rows@, k2, k3, f),
        cell_keys(r@) == cell_keys(rows@),
{
    let mut r: Vec<Cell> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> r@[j].k2 == rows@[j].k2 && r@[j].k3 == rows@[j].k3 && r@[j].a@ == rows@[j].a@ && r@[j].b@ == rows@[j].b@ && r@[j].c@ == rows@[j].c@,
        decreases rows.len() - i,
    {
        r.push(Cell { k2: rows[i].k2, k3: rows[i].k3, a: rows[i].a.dup(), b: rows[i].b.dup(), c: rows[i].c.dup() });
        i += 1;
    }
    proof {
        assert forall|k2: int, k3: int, f: int| #[trigger] cell_at(r@, k2, k3, f) == cell_at(rows@, k2, k3, f) by {
            lemma_sum_ext_pointwise(r@, rows@, cell_w(k2, k3, f), cell_w(k2, k3, f));
        }
        assert(cell_keys(r@) =~= cell_keys(rows@));
    }
    r
}

proof fn lemma_sum_ext_pointwise(s: Seq<Cell>, t: Seq<Cell>, f: spec_fn(Cell) -> int, g: spec_fn(Cell) -> int)
    requires s.len() == t.len(), forall|j: int| 0 <= j < s.len() ==> f(s[j]) == g(t[j]),
    ensures sum(s, f) == sum(t, g),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_ext_pointwise(s.drop_last(), t.drop_last(), f, g);
    }
}

/// rows += (k2, k3: da, db, dc).
pub fn bump_cell(rows: &mut Vec<Cell>, k2: i64, k3: i64, da: &Wide, db: &Wide, dc: &Wide)
    ensures
        forall|j2: int, j3: int, f: int| 0 <= f <= 2 ==> #[trigger] cell_at(final(rows)@, j2, j3, f)
            == cell_at(old(rows)@, j2, j3, f) + if j2 == k2 as int && j3 == k3 as int { d3(da@, db@, dc@, f) } else { 0 },
        cell_keys(old(rows)@).no_duplicates() ==> cell_keys(final(rows)@).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> !((#[trigger] rows@[position]).k2 == k2 && rows@[position].k3 == k3),
        decreases rows.len() - index,
    {
        if rows[index].k2 == k2 && rows[index].k3 == k3 {
            let entry = Cell { k2, k3, a: rows[index].a.add(da), b: rows[index].b.add(db), c: rows[index].c.add(dc) };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|j2: int, j3: int, f: int| 0 <= f <= 2 implies #[trigger] cell_at(rows@, j2, j3, f)
                    == cell_at(before, j2, j3, f) + if j2 == k2 as int && j3 == k3 as int { d3(da@, db@, dc@, f) } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, cell_w(j2, j3, f));
                }
                assert(cell_keys(rows@) =~= cell_keys(before));
            }
            return;
        }
        index += 1;
    }
    let entry = Cell { k2, k3, a: da.dup(), b: db.dup(), c: dc.dup() };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|j2: int, j3: int, f: int| 0 <= f <= 2 implies #[trigger] cell_at(rows@, j2, j3, f)
            == cell_at(before, j2, j3, f) + if j2 == k2 as int && j3 == k3 as int { d3(da@, db@, dc@, f) } else { 0 } by {
            lemma_sum_push(before, appended, cell_w(j2, j3, f));
        }
        if cell_keys(before).no_duplicates() {
            assert(!cell_keys(before).contains((k2, k3)));
            verus_prooflib::lemma_unique_push(cell_keys(before), (k2, k3));
            assert(cell_keys(rows@) =~= cell_keys(before).push((k2, k3)));
        }
    }
}

pub type PMap = Pages<Vec<Cell>>;
pub type Ov = Work<Vec<Cell>>;

pub open spec fn pm_at(pm: PMap, k1: i64, k2: int, k3: int, f: int) -> int {
    match pm.at(k1) { Some(rows) => cell_at(rows@, k2, k3, f), None => 0int }
}

pub open spec fn ov_at(base: PMap, ov: Ov, k1: i64, k2: int, k3: int, f: int) -> int {
    if ov.rows@.contains_key(k1) { cell_at(ov.rows@[k1]@, k2, k3, f) } else { pm_at(base, k1, k2, k3, f) }
}

/// Every bucket has distinct (k2, k3).
pub open spec fn pm_unique(pm: PMap) -> bool {
    forall|k1: i64| #[trigger] pm.at(k1).is_some() ==> cell_keys(pm.at(k1).unwrap()@).no_duplicates()
}

pub open spec fn ov_unique(base: PMap, ov: Ov) -> bool {
    forall|k1: i64| #[trigger] ov.rows@.contains_key(k1) ==> cell_keys(ov.rows@[k1]@).no_duplicates()
}

pub fn pm_empty() -> (r: PMap)
    ensures r.valid(), forall|k: i64| #[trigger] r.at(k).is_none(), pm_unique(r),
{
    let w: Ov = Work::new();
    let r = Pages::from_work(w);
    r
}

/// overlay[(k1, k2, k3)] += (da, db, dc).
pub fn ov_add(base: &PMap, ov: &mut Ov, k1: i64, k2: i64, k3: i64, da: &Wide, db: &Wide, dc: &Wide)
    requires base.valid(), old(ov).valid(), pm_unique(*base), ov_unique(*base, *old(ov)),
    ensures final(ov).valid(), ov_unique(*base, *final(ov)),
        forall|j1: i64, j2: int, j3: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*base, *final(ov), j1, j2, j3, f)
            == ov_at(*base, *old(ov), j1, j2, j3, f) + if j1 == k1 && j2 == k2 as int && j3 == k3 as int { d3(da@, db@, dc@, f) } else { 0 },
{
    let ghost before = *ov;
    let mut rows = match ov.take(k1) {
        Some(rows) => rows,
        None => match base.get(k1) { Some(rows) => copy_cells(&rows), None => Vec::new() },
    };
    proof {
        if !before.rows@.contains_key(k1) {
            if base.at(k1).is_none() {
                assert forall|j2: int, j3: int, f: int| #[trigger] cell_at(rows@, j2, j3, f) == ov_at(*base, before, k1, j2, j3, f) by {
                    lemma_sum_empty(rows@, cell_w(j2, j3, f));
                }
                assert(cell_keys(rows@) =~= Seq::<(i64, i64)>::empty());
            }
        }
    }
    bump_cell(&mut rows, k2, k3, da, db, dc);
    ov.put(k1, rows);
}

/// The bucket of k1 in the overlay-over-base view, copied.
pub fn ov_bucket(base: &PMap, ov: &Ov, k1: i64) -> (r: Vec<Cell>)
    requires base.valid(),
    ensures forall|j2: int, j3: int, f: int| #[trigger] cell_at(r@, j2, j3, f) == ov_at(*base, *ov, k1, j2, j3, f),
{
    match ov.rows.get(&k1) {
        Some(rows) => copy_cells(rows),
        None => match base.get(k1) {
            Some(rows) => copy_cells(&rows),
            None => {
                let r: Vec<Cell> = Vec::new();
                proof {
                    assert forall|j2: int, j3: int, f: int| #[trigger] cell_at(r@, j2, j3, f) == ov_at(*base, *ov, k1, j2, j3, f) by {
                        lemma_sum_empty(r@, cell_w(j2, j3, f));
                    }
                }
                r
            }
        },
    }
}

/// The overlay applied to the base.
pub fn publish(base: &PMap, ov: Ov) -> (r: PMap)
    requires base.valid(), ov.valid(), pm_unique(*base), ov_unique(*base, ov),
    ensures r.valid(), pm_unique(r),
        forall|k1: i64, j2: int, j3: int, f: int| #[trigger] pm_at(r, k1, j2, j3, f) == ov_at(*base, ov, k1, j2, j3, f),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let mut next = base.share();
    let mut work = ov;
    let ghost initial = work;
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= initial.keys.len(), initial.valid(), work.valid(), next.valid(), work.keys@ == initial.keys@,
            pm_unique(*base), ov_unique(*base, initial),
            forall|key: i64| #[trigger] work.rows@.contains_key(key)
                <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] next.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                Some(initial.rows@[key])
            } else { base.at(key) },
        decreases initial.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let rows = work.take(key);
        match rows { Some(rows) => next.put(key, Some(rows)), None => {} }
        proof {
            assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            assert forall|target: i64| #[trigger] next.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                Some(initial.rows@[target])
            } else { base.at(target) } by {}
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target)
                <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {}
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|k1: i64| #[trigger] next.at(k1).is_some() implies cell_keys(next.at(k1).unwrap()@).no_duplicates() by {
            if initial.rows@.contains_key(k1) { assert(initial.keys@.contains(k1)); }
        }
        assert forall|k1: i64, j2: int, j3: int, f: int| #[trigger] pm_at(next, k1, j2, j3, f) == ov_at(*base, initial, k1, j2, j3, f) by {
            if initial.rows@.contains_key(k1) { assert(initial.keys@.contains(k1)); }
        }
    }
    next
}

}
