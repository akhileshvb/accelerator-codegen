use vstd::prelude::*;
use crate::key_facts::lemma_rank_key_injective;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, Index};
use crate::gen_spec::{CustomerRow, NationRow};
use crate::gen_obligations::{CustomerInput, NationInput, customer_input_view, nation_input_view};
use crate::values::*;
use crate::key_facts::lemma_customer_key_injective;

verus! {

pub struct Customer {
    pub row: Arc<CustomerInput>,
    pub count: i128,
}

pub struct Nation {
    pub row: Arc<NationInput>,
    pub count: i128,
}

pub struct Dimensions {
    pub customers: Index<Index<Customer>>,
    pub nations: Index<Index<Nation>>,
    pub customer_totals: Index<i128>,
    pub nation_totals: Index<i128>,
    pub customer_maxima: Index<i128>,
    pub nation_maxima: Index<i128>,
}

pub open spec fn customer_ref_view(row: &CustomerInput) -> CustomerRow { customer_input_view(*row) }

pub open spec fn customer_leaf_valid(inner: Index<Customer>, customer: int) -> bool {
    &&& inner.wf()
    &&& forall|key: (int, Seq<u8>)| #[trigger] inner.at(key).is_some() ==> {
        let value = inner.at(key).unwrap();
        &&& value.count != 0
        &&& customer_input_view(*value.row).c_custkey == customer
        &&& customer_inner_view(customer_input_view(*value.row)) == key
    }
}

pub open spec fn nation_leaf_valid(inner: Index<Nation>, nation: int) -> bool {
    &&& inner.wf()
    &&& forall|key: (int, Seq<u8>)| #[trigger] inner.at(key).is_some() ==> {
        let value = inner.at(key).unwrap();
        &&& value.count != 0
        &&& nation_input_view(*value.row).n_nationkey == nation
        &&& nation_inner_view(nation_input_view(*value.row)) == key
    }
}

pub open spec fn customer_index_valid(index: Index<Index<Customer>>) -> bool {
    forall|outer: (int, Seq<u8>)| #[trigger] index.at(outer).is_some() ==> {
        let inner = index.at(outer).unwrap();
        &&& outer.1 == Seq::<u8>::empty()
        &&& inner.entries().len() > 0
        &&& customer_leaf_valid(inner, outer.0)
    }
}

pub open spec fn nation_index_valid(index: Index<Index<Nation>>) -> bool {
    forall|outer: (int, Seq<u8>)| #[trigger] index.at(outer).is_some() ==> {
        let inner = index.at(outer).unwrap();
        &&& outer.1 == Seq::<u8>::empty()
        &&& inner.entries().len() > 0
        &&& nation_leaf_valid(inner, outer.0)
    }
}

pub open spec fn customer_inner_view(row: CustomerRow) -> (int, Seq<u8>) {
    (row.c_nationkey, encode_text(row.c_name) + encode_text(row.c_address)
        + encode_text(row.c_phone) + encode_nat((row.c_acctbal - i64::MIN as int) as nat, 8)
        + encode_text(row.c_comment))
}

pub open spec fn nation_inner_view(row: NationRow) -> (int, Seq<u8>) { (0, row.n_name) }

pub open spec fn customer_count(index: Index<Index<Customer>>, row: CustomerRow) -> int {
    match index.at((row.c_custkey, Seq::empty())) {
        Some(inner) => match inner.at(customer_inner_view(row)) {
            Some(value) => if customer_input_view(*value.row) == row { value.count as int } else { 0 },
            None => 0,
        },
        None => 0,
    }
}

pub open spec fn nation_count(index: Index<Index<Nation>>, row: NationRow) -> int {
    match index.at((row.n_nationkey, Seq::empty())) {
        Some(inner) => match inner.at(nation_inner_view(row)) {
            Some(value) => if nation_input_view(*value.row) == row { value.count as int } else { 0 },
            None => 0,
        },
        None => 0,
    }
}

pub open spec fn total_at(index: Index<i128>, key: int) -> int {
    match index.at((key, Seq::empty())) { Some(value) => value as int, None => 0 }
}

pub fn customer_key(row: &CustomerInput) -> (result: Key)
    ensures result.view() == customer_inner_view(customer_input_view(*row)),
{
    let mut bytes = Vec::new();
    append_text(&mut bytes, row.c_name.as_slice());
    append_text(&mut bytes, row.c_address.as_slice());
    append_text(&mut bytes, row.c_phone.as_slice());
    append_i64(&mut bytes, row.c_acctbal);
    append_text(&mut bytes, row.c_comment.as_slice());
    Key::composite(row.c_nationkey, bytes)
}

pub fn copy_customer(row: &CustomerInput) -> (result: Arc<CustomerInput>)
    ensures customer_input_view(*result) == customer_input_view(*row),
{
    Arc::new(CustomerInput {
        c_custkey: row.c_custkey,
        c_nationkey: row.c_nationkey,
        c_acctbal: row.c_acctbal,
        c_name: copy_bytes(row.c_name.as_slice()),
        c_address: copy_bytes(row.c_address.as_slice()),
        c_phone: copy_bytes(row.c_phone.as_slice()),
        c_comment: copy_bytes(row.c_comment.as_slice()),
    })
}

pub fn copy_nation(row: &NationInput) -> (result: Arc<NationInput>)
    ensures nation_input_view(*result) == nation_input_view(*row),
{
    Arc::new(NationInput { n_nationkey: row.n_nationkey, n_name: copy_bytes(row.n_name.as_slice()) })
}

#[verifier::rlimit(20)]
pub fn change_customer(index: &mut Index<Index<Customer>>, row: &CustomerInput, change: i128) -> (result: bool)
    requires customer_index_valid(*old(index)),
    ensures result ==> customer_index_valid(*final(index)),
        result ==> forall|target: CustomerRow| #[trigger] customer_count(*final(index), target)
            == customer_count(*old(index), target) + if target == customer_input_view(*row) { change as int } else { 0 },
{
    let ghost before = *index;
    let outer_key = Key::number(row.c_custkey);
    let mut inner = match index.get(&outer_key) {
        Some(value) => {
            let shared = value.share();
            proof {
                assert(before.at(outer_key.view()) == Some(*value));
                assert(customer_leaf_valid(*value, row.c_custkey as int));
                assert forall|key: (int, Seq<u8>)| #[trigger] shared.at(key) == value.at(key) by {}
                assert(customer_leaf_valid(shared, row.c_custkey as int));
            }
            shared
        },
        None => Index::new(),
    };
    let ghost before_inner = inner;
    proof {
        assert(customer_leaf_valid(inner, row.c_custkey as int));
        assert forall|key: (int, Seq<u8>)| #[trigger] inner.at(key)
            == match before.at(outer_key.view()) { Some(original) => original.at(key), None => None } by {}
    }
    let key = customer_key(row);
    let (old, payload) = match inner.get(&key) {
        Some(value) => {
            let ghost stored = customer_ref_view(&*value.row);
            let ghost incoming = customer_ref_view(row);
            proof { lemma_customer_key_injective(stored, incoming); }
            (value.count, value.row.clone())
        },
        None => (0i128, copy_customer(row)),
    };
    proof { assert(customer_input_view(*payload) == customer_input_view(*row)); }
    let count = match old.checked_add(change) { Some(value) => value, None => return false };
    if count == 0 { inner.remove(&key); }
    else { inner.put(key, Arc::new(Customer { row: payload, count })); }
    proof { inner.model(); }
    proof {
        assert forall|target_key: (int, Seq<u8>)| #[trigger] inner.at(target_key).is_some() implies {
            let value = inner.at(target_key).unwrap();
            &&& value.count != 0
            &&& customer_input_view(*value.row).c_custkey == row.c_custkey as int
            &&& customer_inner_view(customer_input_view(*value.row)) == target_key
        } by {
            if target_key != key.view() { assert(inner.at(target_key) == before_inner.at(target_key)); }
        }
    }
    let ghost new_inner = inner;
    if inner.is_empty() { index.remove(&outer_key); }
    else { index.put(outer_key, Arc::new(inner)); }
    proof {
        assert forall|target: CustomerRow| #[trigger] customer_count(*index, target)
            == customer_count(before, target) + if target == customer_input_view(*row) { change as int } else { 0 } by {
                if target.c_custkey != row.c_custkey {
                    assert(index.at((target.c_custkey, Seq::empty())) == before.at((target.c_custkey, Seq::empty())));
                } else if target == customer_input_view(*row) {
                    assert(customer_count(before, target) == old as int);
                } else if customer_inner_view(target) == key.view() {
                    assert(customer_count(before, target) == 0);
                    assert(customer_count(*index, target) == 0);
                }
            }
    }
    true
}

pub fn change_nation(index: &mut Index<Index<Nation>>, row: &NationInput, change: i128) -> (result: bool)
    requires nation_index_valid(*old(index)),
    ensures result ==> nation_index_valid(*final(index)),
        result ==> forall|target: NationRow| #[trigger] nation_count(*final(index), target)
            == nation_count(*old(index), target) + if target == nation_input_view(*row) { change as int } else { 0 },
{
    let ghost before = *index;
    let outer_key = Key::number(row.n_nationkey);
    let mut inner = match index.get(&outer_key) {
        Some(value) => {
            let shared = value.share();
            proof {
                assert(before.at(outer_key.view()) == Some(*value));
                assert(nation_leaf_valid(*value, row.n_nationkey as int));
                assert forall|key: (int, Seq<u8>)| #[trigger] shared.at(key) == value.at(key) by {}
                assert(nation_leaf_valid(shared, row.n_nationkey as int));
            }
            shared
        },
        None => Index::new(),
    };
    let ghost before_inner = inner;
    proof {
        assert(nation_leaf_valid(inner, row.n_nationkey as int));
        assert forall|key: (int, Seq<u8>)| #[trigger] inner.at(key)
            == match before.at(outer_key.view()) { Some(original) => original.at(key), None => None } by {}
    }
    let key = Key::text(copy_bytes(row.n_name.as_slice()));
    let (old, payload) = match inner.get(&key) {
        Some(value) => (value.count, value.row.clone()),
        None => (0i128, copy_nation(row)),
    };
    proof { assert(nation_input_view(*payload) == nation_input_view(*row)); }
    let count = match old.checked_add(change) { Some(value) => value, None => return false };
    if count == 0 { inner.remove(&key); }
    else { inner.put(key, Arc::new(Nation { row: payload, count })); }
    proof { inner.model(); }
    proof {
        assert forall|target_key: (int, Seq<u8>)| #[trigger] inner.at(target_key).is_some() implies {
            let value = inner.at(target_key).unwrap();
            &&& value.count != 0
            &&& nation_input_view(*value.row).n_nationkey == row.n_nationkey as int
            &&& nation_inner_view(nation_input_view(*value.row)) == target_key
        } by {
            if target_key != key.view() { assert(inner.at(target_key) == before_inner.at(target_key)); }
        }
    }
    let ghost new_inner = inner;
    if inner.is_empty() { index.remove(&outer_key); }
    else { index.put(outer_key, Arc::new(inner)); }
    proof {
        assert forall|target: NationRow| #[trigger] nation_count(*index, target)
            == nation_count(before, target) + if target == nation_input_view(*row) { change as int } else { 0 } by {
                if target.n_nationkey != row.n_nationkey {
                    assert(index.at((target.n_nationkey, Seq::empty())) == before.at((target.n_nationkey, Seq::empty())));
                } else if nation_inner_view(target) == key.view() {
                    assert(target == nation_input_view(*row));
                    assert(nation_count(before, target) == old as int);
                    assert(nation_count(*index, target) == count as int);
                } else {
                    assert(new_inner.at(nation_inner_view(target)) == before_inner.at(nation_inner_view(target)));
                    assert(nation_count(*index, target) == nation_count(before, target));
                }
            }
    }
    true
}

#[verifier::rlimit(20)]
pub fn edit_total(totals: &mut Index<i128>, maxima: &mut Index<i128>, key: i64, change: i128) -> (result: bool)
    requires crate::model::maxima_valid(*old(totals), *old(maxima)),
    ensures result ==> crate::model::maxima_valid(*final(totals), *final(maxima)),
        result ==> forall|target: int| #[trigger] total_at(*final(totals), target)
            == total_at(*old(totals), target) + if target == key { change as int } else { 0 },
{
    let ghost before_totals = *totals;
    let ghost before_maxima = *maxima;
    let lookup = Key::number(key);
    let old = match totals.get(&lookup) { Some(value) => *value, None => 0i128 };
    let next = match old.checked_add(change) { Some(value) => value, None => return false };
    if next < 0 { return false; }
    if old > 0 { maxima.remove(&rank_key(old, key)); }
    if next == 0 { totals.remove(&lookup); }
    else {
        totals.put(lookup, Arc::new(next));
        maxima.put(rank_key(next, key), Arc::new(next));
    }
    proof {
        assert forall|target: (int, Seq<u8>)| #[trigger] totals.at(target).is_some() implies {
            let value = totals.at(target).unwrap();
            &&& target.1 == Seq::<u8>::empty() && i64::MIN <= target.0 <= i64::MAX
            &&& value > 0
            &&& maxima.at(rank_view(value as int, target.0)) == Some(value)
        } by {
            if target != lookup.view() {
                let value = totals.at(target).unwrap();
                assert(before_totals.at(target) == Some(value));
                if rank_view(value as int, target.0) == rank_view(old as int, key as int) {
                    lemma_rank_key_injective(value, target.0 as i64, old, key);
                }
                if rank_view(value as int, target.0) == rank_view(next as int, key as int) {
                    lemma_rank_key_injective(value, target.0 as i64, next, key);
                }
            }
        }
        assert forall|target: (int, Seq<u8>)| #[trigger] maxima.at(target).is_some() implies {
            let value = maxima.at(target).unwrap();
            &&& value > 0
            &&& exists|join_key: int| #[trigger] totals.at((join_key, Seq::empty())) == Some(value)
                && target == rank_view(value as int, join_key)
        } by {
            let value = maxima.at(target).unwrap();
            if next > 0 && target == rank_view(next as int, key as int) {
                assert(totals.at((key as int, Seq::empty())) == Some(value));
            } else {
                assert(before_maxima.at(target) == Some(value));
                let join_key = choose|join_key: int| #[trigger] before_totals.at((join_key, Seq::empty())) == Some(value)
                    && target == rank_view(value as int, join_key);
                if join_key == key {
                    assert(value == old);
                }
                assert(totals.at((join_key, Seq::empty())) == Some(value));
            }
        }
    }
    true
}

impl Dimensions {
    pub fn new() -> (result: Self)
        ensures customer_index_valid(result.customers), nation_index_valid(result.nations),
            crate::model::maxima_valid(result.customer_totals, result.customer_maxima),
            crate::model::maxima_valid(result.nation_totals, result.nation_maxima),
            forall|key: int| #[trigger] total_at(result.customer_totals, key) == 0,
            forall|key: int| #[trigger] total_at(result.nation_totals, key) == 0,
            forall|row: CustomerRow| #[trigger] customer_count(result.customers, row) == 0,
            forall|row: NationRow| #[trigger] nation_count(result.nations, row) == 0,
    {
        Self { customers: Index::new(), nations: Index::new(), customer_totals: Index::new(),
            nation_totals: Index::new(), customer_maxima: Index::new(), nation_maxima: Index::new() }
    }

    pub fn share(&self) -> (result: Self)
        ensures result.customers.tree() == self.customers.tree(), result.nations.tree() == self.nations.tree(),
            result.customer_totals.tree() == self.customer_totals.tree(), result.nation_totals.tree() == self.nation_totals.tree(),
            result.customer_maxima.tree() == self.customer_maxima.tree(), result.nation_maxima.tree() == self.nation_maxima.tree(),
            crate::model::maxima_valid(self.customer_totals, self.customer_maxima)
                ==> crate::model::maxima_valid(result.customer_totals, result.customer_maxima),
            crate::model::maxima_valid(self.nation_totals, self.nation_maxima)
                ==> crate::model::maxima_valid(result.nation_totals, result.nation_maxima),
    {
        let result = Self { customers: self.customers.share(), nations: self.nations.share(),
            customer_totals: self.customer_totals.share(), nation_totals: self.nation_totals.share(),
            customer_maxima: self.customer_maxima.share(), nation_maxima: self.nation_maxima.share() };
        proof {
            assert forall|key: (int, Seq<u8>)| #[trigger] result.customer_totals.at(key) == self.customer_totals.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.nation_totals.at(key) == self.nation_totals.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.customer_maxima.at(key) == self.customer_maxima.at(key) by {}
            assert forall|key: (int, Seq<u8>)| #[trigger] result.nation_maxima.at(key) == self.nation_maxima.at(key) by {}
            if crate::model::maxima_valid(self.customer_totals, self.customer_maxima) {
                crate::model::lemma_maxima_equal(self.customer_totals, self.customer_maxima, result.customer_totals, result.customer_maxima);
            }
            if crate::model::maxima_valid(self.nation_totals, self.nation_maxima) {
                crate::model::lemma_maxima_equal(self.nation_totals, self.nation_maxima, result.nation_totals, result.nation_maxima);
            }
        }
        result
    }

    pub fn edit_customer(&mut self, row: &CustomerInput, change: i128) -> (result: bool)
        requires customer_index_valid(old(self).customers),
            crate::model::maxima_valid(old(self).customer_totals, old(self).customer_maxima),
        ensures result ==> customer_index_valid(final(self).customers),
            result ==> forall|target: CustomerRow| #[trigger] customer_count(final(self).customers, target)
                == customer_count(old(self).customers, target) + if target == customer_input_view(*row) { change as int } else { 0 },
            final(self).nations.tree() == old(self).nations.tree(),
            nation_index_valid(old(self).nations) ==> nation_index_valid(final(self).nations),
            forall|target: NationRow| #[trigger] nation_count(final(self).nations, target) == nation_count(old(self).nations, target),
            result ==> crate::model::maxima_valid(final(self).customer_totals, final(self).customer_maxima),
            result ==> forall|key: int| #[trigger] total_at(final(self).customer_totals, key)
                == total_at(old(self).customer_totals, key) + if key == row.c_custkey { change as int } else { 0 },
            final(self).nation_totals.tree() == old(self).nation_totals.tree(),
            final(self).nation_maxima.tree() == old(self).nation_maxima.tree(),
            crate::model::maxima_valid(old(self).nation_totals, old(self).nation_maxima)
                ==> crate::model::maxima_valid(final(self).nation_totals, final(self).nation_maxima),
            forall|key: int| #[trigger] total_at(final(self).nation_totals, key) == total_at(old(self).nation_totals, key),
    {
        let old = match self.customers.get(&Key::number(row.c_custkey)) {
            Some(inner) => match inner.get(&customer_key(row)) { Some(value) => value.count, None => 0i128 },
            None => 0i128,
        };
        let next = match old.checked_add(change) { Some(value) => value, None => return false };
        if next < 0 { return false; }
        change_customer(&mut self.customers, row, change)
            && edit_total(&mut self.customer_totals, &mut self.customer_maxima, row.c_custkey, change)
    }

    pub fn edit_nation(&mut self, row: &NationInput, change: i128) -> (result: bool)
        requires nation_index_valid(old(self).nations),
            crate::model::maxima_valid(old(self).nation_totals, old(self).nation_maxima),
        ensures result ==> nation_index_valid(final(self).nations),
            result ==> forall|target: NationRow| #[trigger] nation_count(final(self).nations, target)
                == nation_count(old(self).nations, target) + if target == nation_input_view(*row) { change as int } else { 0 },
            final(self).customers.tree() == old(self).customers.tree(),
            customer_index_valid(old(self).customers) ==> customer_index_valid(final(self).customers),
            forall|target: CustomerRow| #[trigger] customer_count(final(self).customers, target) == customer_count(old(self).customers, target),
            result ==> crate::model::maxima_valid(final(self).nation_totals, final(self).nation_maxima),
            result ==> forall|key: int| #[trigger] total_at(final(self).nation_totals, key)
                == total_at(old(self).nation_totals, key) + if key == row.n_nationkey { change as int } else { 0 },
            final(self).customer_totals.tree() == old(self).customer_totals.tree(),
            final(self).customer_maxima.tree() == old(self).customer_maxima.tree(),
            crate::model::maxima_valid(old(self).customer_totals, old(self).customer_maxima)
                ==> crate::model::maxima_valid(final(self).customer_totals, final(self).customer_maxima),
            forall|key: int| #[trigger] total_at(final(self).customer_totals, key) == total_at(old(self).customer_totals, key),
    {
        let old = match self.nations.get(&Key::number(row.n_nationkey)) {
            Some(inner) => match inner.get(&Key::text(copy_bytes(row.n_name.as_slice()))) {
                Some(value) => value.count, None => 0i128,
            },
            None => 0i128,
        };
        let next = match old.checked_add(change) { Some(value) => value, None => return false };
        if next < 0 { return false; }
        change_nation(&mut self.nations, row, change)
            && edit_total(&mut self.nation_totals, &mut self.nation_maxima, row.n_nationkey, change)
    }
}

}
