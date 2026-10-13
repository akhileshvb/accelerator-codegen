use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, Index};
use crate::values::*;
use crate::paged::*;
use crate::ents::*;

verus! {

pub struct Slice {
    pub cells: Paged<CellEnt>,
    pub ranks: Paged<RankEnt>,
}

pub open spec fn cell_moments(cell: CellEnt) -> Moments {
    Moments { count: cell.count as i128, revenue: cell.revenue as i128, upper: cell.upper as i128 }
}

pub open spec fn cell_at(cells: Paged<CellEnt>, customer: int) -> Moments {
    match cells.at((customer, Seq::<u8>::empty())) {
        Some(cell) => cell_moments(cell),
        None => Moments { count: 0, revenue: 0, upper: 0 },
    }
}

pub open spec fn rank_of(cell: CellEnt) -> RankEnt { RankEnt { upper: cell.upper, customer: cell.customer } }

pub struct Periods {
    pub quarters: Index<Slice>,
    /// quarters shifted by one and by two months (grains 4 and 5): any window of three whole months is a slice of one of the three quarter grains
    pub quarters1: Index<Slice>,
    pub quarters2: Index<Slice>,
}

impl Periods {
    pub fn new() -> (result: Self)
        ensures slices_valid(result.quarters),
            slices_valid(result.quarters1), slices_valid(result.quarters2),
            forall|key: (int, Seq<u8>)| #[trigger] result.quarters1.at(key) == None,
            forall|key: (int, Seq<u8>)| #[trigger] result.quarters2.at(key) == None,
            forall|key: (int, Seq<u8>)| #[trigger] result.quarters.at(key) == None,
    {
        Self { quarters: Index::new(), quarters1: Index::new(), quarters2: Index::new() }
    }

    pub fn share(&self) -> (result: Self)
        ensures result.quarters.tree() == self.quarters.tree(),
            result.quarters1.tree() == self.quarters1.tree(), result.quarters2.tree() == self.quarters2.tree(),
            slices_valid(self.quarters1) ==> slices_valid(result.quarters1),
            slices_valid(self.quarters2) ==> slices_valid(result.quarters2),
            slices_valid(self.quarters) ==> slices_valid(result.quarters),
    {
        let result = Self { quarters: self.quarters.share(),
            quarters1: self.quarters1.share(), quarters2: self.quarters2.share() };
        proof {
            assert forall|key: (int, Seq<u8>)| #[trigger] result.quarters.at(key) == self.quarters.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.quarters1.at(key) == self.quarters1.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.quarters2.at(key) == self.quarters2.at(key) by {}
        }
        result
    }
}

impl Slice {
    pub fn new() -> (result: Self)
        ensures result.valid(), result.cells.wf(), result.ranks.wf(),
            forall|key: (int, Seq<u8>)| #[trigger] result.cells.at(key) == None::<CellEnt>,
            forall|key: (int, Seq<u8>)| #[trigger] result.ranks.at(key) == None::<RankEnt>,
    { Self { cells: Paged::new(), ranks: Paged::new() } }

    pub fn share(&self) -> (result: Self)
        requires self.valid(),
        ensures result.valid(),
            forall|key: (int, Seq<u8>)| #[trigger] result.cells.at(key) == self.cells.at(key),
            forall|key: (int, Seq<u8>)| #[trigger] result.ranks.at(key) == self.ranks.at(key),
    {
        let result = Self { cells: self.cells.share(), ranks: self.ranks.share() };
        proof {
            assert(result.cells.wf() && result.ranks.wf());
            assert(result.valid());
        }
        result
    }

    pub open spec fn valid(self) -> bool {
        &&& self.cells.wf() && self.ranks.wf()
        &&& forall|key: (int, Seq<u8>)| #[trigger] self.cells.at(key).is_some() ==> {
            let cell = self.cells.at(key).unwrap();
            &&& cell.count > 0 && cell.upper >= 0 && cell.revenue <= cell.upper
            &&& self.ranks.at(rank_of(cell).kv()) == Some(rank_of(cell))
        }
        &&& forall|key: (int, Seq<u8>)| #[trigger] self.ranks.at(key).is_some() ==> {
            let rank = self.ranks.at(key).unwrap();
            &&& rank.upper >= 0
            &&& match self.cells.at((rank.customer as int, Seq::<u8>::empty())) {
                Some(cell) => cell.upper == rank.upper,
                None => false,
            }
        }
    }
}

pub open spec fn slice_at(slices: Index<Slice>, day: int, customer: int) -> Moments {
    match slices.at((day, Seq::empty())) {
        Some(slice) => cell_at(slice.cells, customer),
        None => Moments { count: 0, revenue: 0, upper: 0 },
    }
}

pub open spec fn slices_valid(slices: Index<Slice>) -> bool {
    forall|key: (int, Seq<u8>)| #[trigger] slices.at(key).is_some() ==> {
        &&& key.1 == Seq::<u8>::empty()
        &&& slices.at(key).unwrap().cells.nonempty()
        &&& slices.at(key).unwrap().valid()
    }
}

pub fn probe_cell(customer: i64) -> (result: CellEnt)
    ensures result.customer == customer, result.kv() == (customer as int, Seq::<u8>::empty()),
{ CellEnt { customer, count: 0, revenue: 0, upper: 0 } }

pub proof fn lemma_cell_key(cells: Paged<CellEnt>, key: (int, Seq<u8>))
    requires cells.wf(), cells.at(key).is_some(),
    ensures cells.at(key).unwrap().kv() == key, key == (cells.at(key).unwrap().customer as int, Seq::<u8>::empty()),
{
    lemma_at(cells, key, cells.at(key).unwrap());
}

pub proof fn lemma_rank_key(ranks: Paged<RankEnt>, key: (int, Seq<u8>))
    requires ranks.wf(), ranks.at(key).is_some(),
    ensures ranks.at(key).unwrap().kv() == key,
{
    lemma_at(ranks, key, ranks.at(key).unwrap());
}

pub proof fn lemma_edit_valid(before: Slice, after: Slice, customer: i64, old: Moments, next: Moments, ranks_removed: Paged<RankEnt>)
    requires before.valid(), old == cell_at(before.cells, customer as int),
        next.count >= 0, next.upper >= 0, next.revenue <= next.upper,
        next.count == 0 ==> next.revenue == 0 && next.upper == 0,
        next.count <= u32::MAX as i128, next.upper <= i64::MAX as i128, next.revenue >= i64::MIN as i128,
        after.cells.wf(), after.ranks.wf(), ranks_removed.wf(),
        forall|query: (int, Seq<u8>)| #![trigger ranks_removed.at(query)] ranks_removed.at(query)
            == if old.count > 0 && query == (RankEnt { upper: old.upper as i64, customer }).kv() { None } else { before.ranks.at(query) },
        forall|query: (int, Seq<u8>)| #![trigger after.cells.at(query)] after.cells.at(query)
            == if query == (customer as int, Seq::<u8>::empty()) {
                if next.count == 0 { None }
                else { Some(CellEnt { customer, count: next.count as u32, revenue: next.revenue as i64, upper: next.upper as i64 }) }
            } else { before.cells.at(query) },
        forall|query: (int, Seq<u8>)| #![trigger after.ranks.at(query)] after.ranks.at(query)
            == if next.count > 0 && query == (RankEnt { upper: next.upper as i64, customer }).kv() { Some(RankEnt { upper: next.upper as i64, customer }) }
            else { ranks_removed.at(query) },
    ensures after.valid(), cell_at(after.cells, customer as int) == next,
        forall|target: int| target != customer as int ==> #[trigger] cell_at(after.cells, target) == cell_at(before.cells, target),
{
    let ckey = (customer as int, Seq::<u8>::empty());
    let new_cell = CellEnt { customer, count: next.count as u32, revenue: next.revenue as i64, upper: next.upper as i64 };
    let new_rank = RankEnt { upper: next.upper as i64, customer };
    let old_rank = RankEnt { upper: old.upper as i64, customer };
    // the old cell, when present, is the one counted in `old`
    if old.count > 0 {
        lemma_cell_key(before.cells, ckey);
        assert(before.cells.at(ckey).is_some());
        let cell = before.cells.at(ckey).unwrap();
        assert(cell_moments(cell) == old);
        assert(rank_of(cell) == old_rank);
        assert(before.ranks.at(old_rank.kv()) == Some(old_rank));
    }
    assert forall|target: int| target != customer as int implies #[trigger] cell_at(after.cells, target) == cell_at(before.cells, target) by {
        assert(after.cells.at((target, Seq::<u8>::empty())) == before.cells.at((target, Seq::<u8>::empty())));
    }
    assert(cell_at(after.cells, customer as int) == next);
    assert forall|key: (int, Seq<u8>)| #[trigger] after.cells.at(key).is_some() implies {
        let cell = after.cells.at(key).unwrap();
        &&& cell.count > 0 && cell.upper >= 0 && cell.revenue <= cell.upper
        &&& after.ranks.at(rank_of(cell).kv()) == Some(rank_of(cell))
    } by {
        let cell = after.cells.at(key).unwrap();
        if key == ckey {
            assert(cell == new_cell);
            assert(rank_of(cell) == new_rank);
        } else {
            lemma_cell_key(before.cells, key);
            assert(before.cells.at(key) == Some(cell));
            assert(cell.customer != customer);
            let rank = rank_of(cell);
            assert(before.ranks.at(rank.kv()) == Some(rank));
            lemma_rank_order(rank, old_rank);
            lemma_rank_order(rank, new_rank);
            assert(rank.kv() != old_rank.kv());
            assert(rank.kv() != new_rank.kv());
            assert(ranks_removed.at(rank.kv()) == before.ranks.at(rank.kv()));
        }
    }
    assert forall|key: (int, Seq<u8>)| #[trigger] after.ranks.at(key).is_some() implies {
        let rank = after.ranks.at(key).unwrap();
        &&& rank.upper >= 0
        &&& match after.cells.at((rank.customer as int, Seq::<u8>::empty())) {
            Some(cell) => cell.upper == rank.upper,
            None => false,
        }
    } by {
        let rank = after.ranks.at(key).unwrap();
        lemma_rank_key(after.ranks, key);
        if next.count > 0 && key == new_rank.kv() {
            assert(rank == new_rank);
            assert(after.cells.at(ckey) == Some(new_cell));
        } else {
            assert(ranks_removed.at(key) == Some(rank));
            assert(!(old.count > 0 && key == old_rank.kv()));
            assert(before.ranks.at(key) == Some(rank));
            assert(rank.upper >= 0);
            let rkey = (rank.customer as int, Seq::<u8>::empty());
            assert(match before.cells.at(rkey) { Some(cell) => cell.upper == rank.upper, None => false });
            if rank.customer == customer {
                // it would be the old rank
                let cell = before.cells.at(ckey).unwrap();
                lemma_cell_key(before.cells, ckey);
                assert(cell_moments(cell) == old);
                assert(old.count > 0);
                assert(rank == old_rank);
                assert(false);
            }
            assert(after.cells.at(rkey) == before.cells.at(rkey));
        }
    }
}

/// Add `change` to one customer's cell of a slice (and move its rank entry).
#[verifier::rlimit(60)]
pub fn edit_cell(slice: &mut Slice, customer: i64, change: Moments) -> (result: bool)
    requires old(slice).valid(),
    ensures result ==> final(slice).valid(),
        result ==> forall|target: int, component: int| #[trigger] cell_at(final(slice).cells, target).component(component)
            == cell_at(old(slice).cells, target).component(component)
                + if target == customer as int { change.component(component) } else { 0 },
{
    let ghost before_slice = *slice;
    let probe = probe_cell(customer);
    let old_cell = slice.cells.get(&probe);
    let old = match old_cell {
        Some(cell) => Moments { count: cell.count as i128, revenue: cell.revenue as i128, upper: cell.upper as i128 },
        None => Moments::zero(),
    };
    proof {
        assert(slice.valid());
        assert(old == cell_at(slice.cells, customer as int));
        assert(old.upper >= 0);
    }
    let next = match old.add(change) { Some(value) => value, None => return false };
    if next.count < 0 || next.upper < 0 || next.revenue > next.upper { return false; }
    if next.count == 0 && !next.is_zero() { return false; }
    if next.count > u32::MAX as i128 || next.upper > i64::MAX as i128 || next.revenue < i64::MIN as i128 { return false; }
    let new_cell = CellEnt { customer, count: next.count as u32, revenue: next.revenue as i64, upper: next.upper as i64 };
    let new_rank = RankEnt { upper: next.upper as i64, customer };
    if old.count > 0 { slice.ranks.remove(&RankEnt { upper: old.upper as i64, customer }); }
    let ghost ranks_removed = slice.ranks;
    if next.count == 0 { slice.cells.remove(&probe); }
    else {
        slice.cells.put(new_cell);
        slice.ranks.put(new_rank);
    }
    proof {
        lemma_edit_valid(before_slice, *slice, customer, old, next, ranks_removed);
        assert forall|target: int, component: int| #[trigger] cell_at(slice.cells, target).component(component)
            == cell_at(before_slice.cells, target).component(component)
                + if target == customer as int { change.component(component) } else { 0 } by {
            if target == customer as int { assert(cell_at(slice.cells, target) == next); }
        }
    }
    true
}

#[verifier::rlimit(60)]
pub fn edit_slice(slices: &mut Index<Slice>, day: i64, customer: i64, change: Moments) -> (result: bool)
    requires slices_valid(*old(slices)),
    ensures result ==> slices_valid(*final(slices)),
        result ==> forall|target_day: int, target_customer: int, component: int|
            #[trigger] slice_at(*final(slices), target_day, target_customer).component(component)
                == slice_at(*old(slices), target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change.component(component) } else { 0 },
{
    let ghost before = *slices;
    let key = Key::number(day);
    let mut slice = take_slice(slices, &key, Ghost(day as int));
    let ghost before_slice = slice;
    if !edit_cell(&mut slice, customer, change) { return false; }
    put_slice(slices, key, slice, Ghost(day as int));
    proof {
        assert(slices_valid(*slices));
        assert forall|target_day: int, target_customer: int, component: int|
            #[trigger] slice_at(*slices, target_day, target_customer).component(component)
                == slice_at(before, target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change.component(component) } else { 0 } by {
            if target_day == day {
                assert(cell_at(before_slice.cells, target_customer) == slice_at(before, day as int, target_customer));
            }
        }
    }
    true
}

/// The slice at `key` (a shared copy), or a new one.
pub fn take_slice(slices: &Index<Slice>, key: &Key, Ghost(day): Ghost<int>) -> (slice: Slice)
    requires slices_valid(*slices), key.view() == (day, Seq::<u8>::empty()),
    ensures slice.valid(),
        forall|target: int| #[trigger] cell_at(slice.cells, target) == slice_at(*slices, day, target),
{
    match slices.get(key) {
        Some(value) => {
            let shared = value.share();
            proof {
                assert(slices.at(key.view()) == Some(*value));
                assert(value.valid());
                assert forall|target: int| #[trigger] cell_at(shared.cells, target) == slice_at(*slices, day, target) by {}
            }
            shared
        },
        None => Slice::new(),
    }
}

/// Store the slice back (an empty one removes the key).
pub fn put_slice(slices: &mut Index<Slice>, key: Key, slice: Slice, Ghost(day): Ghost<int>)
    requires slices_valid(*old(slices)), slice.valid(), key.view() == (day, Seq::<u8>::empty()),
    ensures slices_valid(*final(slices)),
        forall|target_day: int, target: int| #[trigger] slice_at(*final(slices), target_day, target)
            == if target_day == day { cell_at(slice.cells, target) } else { slice_at(*old(slices), target_day, target) },
{
    if slice.cells.is_empty() { slices.remove(&key); }
    else { slices.put(key, Arc::new(slice)); }
}

}
