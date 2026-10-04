use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, Index};
use crate::values::*;

verus! {

pub struct Rank {
    pub customer: i64,
    pub moments: Arc<Moments>,
}

pub struct Slice {
    pub cells: Index<Moments>,
    pub ranks: Index<Rank>,
}

pub struct Periods {
    pub months: Index<Slice>,
    pub quarters: Index<Slice>,
    pub years: Index<Slice>,
}

impl Periods {
    pub fn new() -> (result: Self)
        ensures slices_valid(result.months), slices_valid(result.quarters), slices_valid(result.years),
            forall|key: (int, Seq<u8>)| #[trigger] result.months.at(key) == None,
            forall|key: (int, Seq<u8>)| #[trigger] result.quarters.at(key) == None,
            forall|key: (int, Seq<u8>)| #[trigger] result.years.at(key) == None,
    {
        Self { months: Index::new(), quarters: Index::new(), years: Index::new() }
    }

    pub fn share(&self) -> (result: Self)
        ensures result.months.tree() == self.months.tree(), result.quarters.tree() == self.quarters.tree(), result.years.tree() == self.years.tree(),
            slices_valid(self.months) ==> slices_valid(result.months),
            slices_valid(self.quarters) ==> slices_valid(result.quarters),
            slices_valid(self.years) ==> slices_valid(result.years),
    {
        let result = Self { months: self.months.share(), quarters: self.quarters.share(), years: self.years.share() };
        proof {
            assert forall|key: (int, Seq<u8>)| #[trigger] result.months.at(key) == self.months.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.quarters.at(key) == self.quarters.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.years.at(key) == self.years.at(key) by {}
        }
        result
    }
}

impl Slice {
    pub fn new() -> (result: Self)
        ensures result.valid(), result.cells.wf(), result.ranks.wf(),
            forall|key: (int, Seq<u8>)| #[trigger] result.cells.at(key) == None,
            forall|key: (int, Seq<u8>)| #[trigger] result.ranks.at(key) == None,
    { Self { cells: Index::new(), ranks: Index::new() } }
    pub fn share(&self) -> (result: Self)
        ensures result.cells.tree() == self.cells.tree(), result.ranks.tree() == self.ranks.tree(),
            self.valid() ==> result.valid(),
            result.cells.wf(), result.ranks.wf(),
    {
        let result = Self { cells: self.cells.share(), ranks: self.ranks.share() };
        proof {
            assert forall|key: (int, Seq<u8>)| #[trigger] result.cells.at(key) == self.cells.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.ranks.at(key) == self.ranks.at(key) by {}
            if self.valid() {
                assert(result.valid());
            }
        }
        result
    }

    pub open spec fn valid(self) -> bool {
        &&& self.cells.wf() && self.ranks.wf()
        &&& forall|key: (int, Seq<u8>)| #[trigger] self.cells.at(key).is_some() ==> {
            let value = self.cells.at(key).unwrap();
            &&& key.1 == Seq::<u8>::empty()
            &&& i64::MIN <= key.0 <= i64::MAX
            &&& value.count > 0 && value.upper >= 0 && value.revenue <= value.upper
            &&& self.ranks.at(rank_view(value.upper as int, key.0)).is_some()
            &&& self.ranks.at(rank_view(value.upper as int, key.0)).unwrap().customer as int == key.0
            &&& *self.ranks.at(rank_view(value.upper as int, key.0)).unwrap().moments == value
        }
        &&& forall|key: (int, Seq<u8>)| #[trigger] self.ranks.at(key).is_some() ==> {
            let rank = self.ranks.at(key).unwrap();
            &&& rank.moments.upper >= 0
            &&& key == rank_view(rank.moments.upper as int, rank.customer as int)
            &&& self.cells.at((rank.customer as int, Seq::empty())) == Some(*rank.moments)
        }
    }
}

pub open spec fn slice_at(slices: Index<Slice>, day: int, customer: int) -> Moments {
    match slices.at((day, Seq::empty())) {
        Some(slice) => moment_at(slice.cells, customer),
        None => Moments { count: 0, revenue: 0, upper: 0 },
    }
}

pub open spec fn slices_valid(slices: Index<Slice>) -> bool {
    forall|key: (int, Seq<u8>)| #[trigger] slices.at(key).is_some() ==> {
        &&& key.1 == Seq::<u8>::empty()
        &&& slices.at(key).unwrap().cells.entries().len() > 0
        &&& slices.at(key).unwrap().valid()
    }
}

#[verifier::rlimit(30)]
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
    let mut slice = match slices.get(&key) {
        Some(value) => {
            let shared = value.share();
            proof {
                assert(before.at(key.view()) == Some(*value));
                assert forall|target: int| #[trigger] moment_at(shared.cells, target)
                    == slice_at(before, day as int, target) by {}
            }
            shared
        },
        None => Slice::new(),
    };
    let ghost before_slice = slice;
    let old = get_moments(&slice.cells, customer);
    proof {
        assert(slice.valid());
        assert(old.upper >= 0);
        assert forall|target: int| #[trigger] moment_at(slice.cells, target)
            == slice_at(before, day as int, target) by {}
    }
    let next = match old.add(change) { Some(value) => value, None => return false };
    if next.count < 0 || next.upper < 0 || next.revenue > next.upper { return false; }
    if next.count == 0 && !next.is_zero() { return false; }
    if old.count > 0 { slice.ranks.remove(&rank_key(old.upper, customer)); }
    if next.count == 0 { slice.cells.remove(&Key::number(customer)); }
    else {
        let moments = Arc::new(next);
        slice.cells.put(Key::number(customer), moments.clone());
        slice.ranks.put(rank_key(next.upper, customer), Arc::new(Rank { customer, moments }));
    }
    proof {
        slice.cells.model();
        assert forall|cell_key: (int, Seq<u8>)| #[trigger] slice.cells.at(cell_key).is_some() implies {
            let value = slice.cells.at(cell_key).unwrap();
            &&& cell_key.1 == Seq::<u8>::empty()
            &&& i64::MIN <= cell_key.0 <= i64::MAX
            &&& value.count > 0 && value.upper >= 0 && value.revenue <= value.upper
            &&& slice.ranks.at(rank_view(value.upper as int, cell_key.0)).is_some()
            &&& slice.ranks.at(rank_view(value.upper as int, cell_key.0)).unwrap().customer as int == cell_key.0
            &&& *slice.ranks.at(rank_view(value.upper as int, cell_key.0)).unwrap().moments == value
        } by {
            if cell_key != (customer as int, Seq::empty()) {
                let value = slice.cells.at(cell_key).unwrap();
                assert(before_slice.cells.at(cell_key) == Some(value));
                if rank_view(value.upper as int, cell_key.0) == rank_view(old.upper as int, customer as int) {
                    crate::key_facts::lemma_rank_key_injective(value.upper, cell_key.0 as i64, old.upper, customer);
                }
                if rank_view(value.upper as int, cell_key.0) == rank_view(next.upper as int, customer as int) {
                    crate::key_facts::lemma_rank_key_injective(value.upper, cell_key.0 as i64, next.upper, customer);
                }
            }
        }
        assert forall|rank_key: (int, Seq<u8>)| #[trigger] slice.ranks.at(rank_key).is_some() implies {
            let rank = slice.ranks.at(rank_key).unwrap();
            &&& rank.moments.upper >= 0
            &&& rank_key == rank_view(rank.moments.upper as int, rank.customer as int)
            &&& slice.cells.at((rank.customer as int, Seq::empty())) == Some(*rank.moments)
        } by {
            if next.count == 0 || rank_key != rank_view(next.upper as int, customer as int) {
                let rank = slice.ranks.at(rank_key).unwrap();
                assert(before_slice.ranks.at(rank_key) == Some(rank));
                if rank.customer == customer {
                    assert(before_slice.cells.at((customer as int, Seq::empty())) == Some(*rank.moments));
                    assert(*rank.moments == old);
                }
            }
        }
        assert(slice.valid());
    }
    if slice.cells.is_empty() { slices.remove(&key); }
    else { slices.put(key, Arc::new(slice)); }
    proof {
        assert(slices_valid(*slices));
        assert forall|target_day: int, target_customer: int, component: int|
            #[trigger] slice_at(*slices, target_day, target_customer).component(component)
                == slice_at(before, target_day, target_customer).component(component)
                    + if target_day == day && target_customer == customer { change.component(component) } else { 0 } by {
            if target_day == day {
                if target_customer == customer {
                    assert(moment_at(slice.cells, target_customer) == next);
                } else {
                    assert(moment_at(slice.cells, target_customer) == moment_at(before_slice.cells, target_customer));
                }
            }
        }
    }
    true
}

}
