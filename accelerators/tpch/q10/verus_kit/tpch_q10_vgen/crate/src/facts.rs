use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, Index};
use crate::values::*;
use crate::paged::*;
use crate::ents::*;
use verus_prooflib::wide::Wide;

verus! {

pub struct Slice {
    pub cells: Paged<CellEnt>,
    pub ranks: Paged<RankEnt>,
}

pub open spec fn cell_moments(cell: CellEnt) -> Moments {
    cell.moments()
}

/// The rank of an upper bound: bounds beyond i64 share the top rank.
pub open spec fn sat64(value: int) -> int {
    if value > i64::MAX as int { i64::MAX as int } else { value }
}

/// Moments of a set of returned lines are consistent.
pub open spec fn moment_ok(moments: Moments) -> bool {
    &&& moments.count >= 0 && moments.upper >= 0 && moments.revenue <= moments.upper
    &&& moments.count == 0 ==> moments.revenue == 0 && moments.upper == 0
}

pub open spec fn plus(left: Moments, right: Moments) -> Moments {
    Moments { count: left.count + right.count, revenue: left.revenue + right.revenue, upper: left.upper + right.upper }
}

pub proof fn lemma_moments_ext(left: Moments, right: Moments)
    requires left.component(0) == right.component(0), left.component(1) == right.component(1),
        left.component(2) == right.component(2),
    ensures left == right,
{}

pub proof fn lemma_moment_ok_plus(left: Moments, right: Moments)
    requires moment_ok(left), moment_ok(right),
    ensures moment_ok(plus(left, right)),
{}

/// Sums of consistent moments are consistent.
pub proof fn lemma_sum_moment_ok<A>(rows: Seq<A>, count: spec_fn(A) -> int, revenue: spec_fn(A) -> int, upper: spec_fn(A) -> int)
    requires forall|row: A| #[trigger] moment_ok(Moments { count: count(row), revenue: revenue(row), upper: upper(row) }),
    ensures moment_ok(Moments {
        count: verus_common::sums::sum(rows, count),
        revenue: verus_common::sums::sum(rows, revenue),
        upper: verus_common::sums::sum(rows, upper),
    }),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let rest = rows.drop_last();
        lemma_sum_moment_ok(rest, count, revenue, upper);
        verus_common::sums::lemma_sum_push(rest, rows.last(), count);
        verus_common::sums::lemma_sum_push(rest, rows.last(), revenue);
        verus_common::sums::lemma_sum_push(rest, rows.last(), upper);
        assert(rest.push(rows.last()) =~= rows);
        let last = Moments { count: count(rows.last()), revenue: revenue(rows.last()), upper: upper(rows.last()) };
        assert(moment_ok(last));
        lemma_moment_ok_plus(Moments {
            count: verus_common::sums::sum(rest, count),
            revenue: verus_common::sums::sum(rest, revenue),
            upper: verus_common::sums::sum(rest, upper),
        }, last);
    } else {
        verus_common::sums::lemma_sum_empty(rows, count);
        verus_common::sums::lemma_sum_empty(rows, revenue);
        verus_common::sums::lemma_sum_empty(rows, upper);
    }
}

pub fn saturate64(value: &Wide) -> (result: i64)
    requires value@ >= 0,
    ensures result >= 0, result as int == sat64(value@),
{
    match value.to_i128() {
        Some(narrow) => if narrow > i64::MAX as i128 { i64::MAX } else { narrow as i64 },
        None => i64::MAX,
    }
}

pub open spec fn cell_at(cells: Paged<CellEnt>, customer: int) -> Moments {
    match cells.at((customer, Seq::<u8>::empty())) {
        Some(cell) => cell_moments(cell),
        None => Moments { count: 0, revenue: 0, upper: 0 },
    }
}

pub open spec fn rank_of(cell: CellEnt) -> RankEnt {
    RankEnt { upper: sat64(cell.moments().upper) as i64, customer: cell.customer }
}

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
            &&& cell.moments().count > 0 && cell.moments().upper >= 0 && cell.moments().revenue <= cell.moments().upper
            &&& self.ranks.at(rank_of(cell).kv()) == Some(rank_of(cell))
        }
        &&& forall|key: (int, Seq<u8>)| #[trigger] self.ranks.at(key).is_some() ==> {
            let rank = self.ranks.at(key).unwrap();
            &&& rank.upper >= 0
            &&& match self.cells.at((rank.customer as int, Seq::<u8>::empty())) {
                Some(cell) => sat64(cell.moments().upper) == rank.upper as int,
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
        result.moments() == (Moments { count: 0, revenue: 0, upper: 0 }),
{ CellEnt { customer, count: 0, revenue: 0, upper: 0, spill: None } }

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

pub proof fn lemma_edit_valid(before: Slice, after: Slice, customer: i64, old: Moments, next: Moments, new_cell: CellEnt, ranks_removed: Paged<RankEnt>)
    requires before.valid(), old == cell_at(before.cells, customer as int), moment_ok(next),
        new_cell.customer == customer, new_cell.moments() == next,
        after.cells.wf(), after.ranks.wf(), ranks_removed.wf(),
        forall|query: (int, Seq<u8>)| #![trigger ranks_removed.at(query)] ranks_removed.at(query)
            == if old.count > 0 && query == (RankEnt { upper: sat64(old.upper) as i64, customer }).kv() { None } else { before.ranks.at(query) },
        forall|query: (int, Seq<u8>)| #![trigger after.cells.at(query)] after.cells.at(query)
            == if query == (customer as int, Seq::<u8>::empty()) {
                if next.count == 0 { None } else { Some(new_cell) }
            } else { before.cells.at(query) },
        forall|query: (int, Seq<u8>)| #![trigger after.ranks.at(query)] after.ranks.at(query)
            == if next.count > 0 && query == rank_of(new_cell).kv() { Some(rank_of(new_cell)) }
            else { ranks_removed.at(query) },
    ensures after.valid(), cell_at(after.cells, customer as int) == next,
        forall|target: int| target != customer as int ==> #[trigger] cell_at(after.cells, target) == cell_at(before.cells, target),
{
    let ckey = (customer as int, Seq::<u8>::empty());
    let new_rank = rank_of(new_cell);
    let old_rank = RankEnt { upper: sat64(old.upper) as i64, customer };
    // the old cell, when present, is the one counted in `old`
    if old.count > 0 {
        lemma_cell_key(before.cells, ckey);
        assert(before.cells.at(ckey).is_some());
        let cell = before.cells.at(ckey).unwrap();
        assert(cell.moments() == old);
        assert(rank_of(cell) == old_rank);
        assert(before.ranks.at(old_rank.kv()) == Some(old_rank));
    }
    assert forall|target: int| target != customer as int implies #[trigger] cell_at(after.cells, target) == cell_at(before.cells, target) by {
        assert(after.cells.at((target, Seq::<u8>::empty())) == before.cells.at((target, Seq::<u8>::empty())));
    }
    assert(cell_at(after.cells, customer as int) == next);
    assert forall|key: (int, Seq<u8>)| #[trigger] after.cells.at(key).is_some() implies {
        let cell = after.cells.at(key).unwrap();
        &&& cell.moments().count > 0 && cell.moments().upper >= 0 && cell.moments().revenue <= cell.moments().upper
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
            Some(cell) => sat64(cell.moments().upper) == rank.upper as int,
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
            assert(match before.cells.at(rkey) { Some(cell) => sat64(cell.moments().upper) == rank.upper as int, None => false });
            if rank.customer == customer {
                // it would be the old rank
                let cell = before.cells.at(ckey).unwrap();
                lemma_cell_key(before.cells, ckey);
                assert(cell.moments() == old);
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
pub fn edit_cell(slice: &mut Slice, customer: i64, change: &Moms)
    requires old(slice).valid(),
        moment_ok(plus(cell_at(old(slice).cells, customer as int), change@)),
    ensures final(slice).valid(),
        forall|target: int, component: int| #[trigger] cell_at(final(slice).cells, target).component(component)
            == cell_at(old(slice).cells, target).component(component)
                + if target == customer as int { change@.component(component) } else { 0 },
{
    let ghost before_slice = *slice;
    let probe = probe_cell(customer);
    let old_cell = slice.cells.get(&probe);
    let old = match &old_cell {
        Some(cell) => cell.read(),
        None => Moms::zero(),
    };
    proof {
        assert(slice.valid());
        assert(old@ == cell_at(slice.cells, customer as int));
    }
    let next = old.add(change);
    proof {
        lemma_moments_ext(next@, plus(old@, change@));
        assert(old@.upper >= 0);
    }
    let ghost old_value = old@;
    let ghost next_value = next@;
    if old.count.is_pos() { slice.ranks.remove(&RankEnt { upper: saturate64(&old.upper), customer }); }
    let ghost ranks_removed = slice.ranks;
    let ghost mut kept_cell = probe;
    if next.count.is_zero() {
        proof { lemma_moments_ext(probe.moments(), next@); }
        slice.cells.remove(&probe);
    }
    else {
        let new_cell = CellEnt::make(customer, &next);
        let new_rank = RankEnt { upper: saturate64(&next.upper), customer };
        proof { kept_cell = new_cell; }
        slice.cells.put(new_cell);
        slice.ranks.put(new_rank);
    }
    proof {
        lemma_edit_valid(before_slice, *slice, customer, old_value, next_value, kept_cell, ranks_removed);
        assert forall|target: int, component: int| #[trigger] cell_at(slice.cells, target).component(component)
            == cell_at(before_slice.cells, target).component(component)
                + if target == customer as int { change@.component(component) } else { 0 } by {
            if target == customer as int { assert(cell_at(slice.cells, target) == next@); }
        }
    }
}

#[verifier::rlimit(60)]
pub fn edit_slice(slices: &mut Index<Slice>, day: i64, customer: i64, change: &Moms)
    requires slices_valid(*old(slices)),
        moment_ok(plus(slice_at(*old(slices), day as int, customer as int), change@)),
    ensures slices_valid(*final(slices)),
        forall|target_day: int, target_customer: int, component: int|
            #[trigger] slice_at(*final(slices), target_day, target_customer).component(component)
                == slice_at(*old(slices), target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change@.component(component) } else { 0 },
{
    let ghost before = *slices;
    let key = Key::number(day);
    let mut slice = take_slice(slices, &key, Ghost(day as int));
    let ghost before_slice = slice;
    edit_cell(&mut slice, customer, change);
    put_slice(slices, key, slice, Ghost(day as int));
    proof {
        assert(slices_valid(*slices));
        assert forall|target_day: int, target_customer: int, component: int|
            #[trigger] slice_at(*slices, target_day, target_customer).component(component)
                == slice_at(before, target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change@.component(component) } else { 0 } by {
            if target_day == day {
                assert(cell_at(before_slice.cells, target_customer) == slice_at(before, day as int, target_customer));
            }
        }
    }
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
