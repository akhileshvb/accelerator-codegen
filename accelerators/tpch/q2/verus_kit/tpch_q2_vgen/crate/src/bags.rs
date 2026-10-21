use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
pub use verus_common::bag::BagRow;
use crate::gen_obligations::*;

verus! {

pub fn copy_bytes(source: &[u8]) -> (result: Vec<u8>)
    ensures result@ == source@,
{
    let mut result = Vec::with_capacity(source.len());
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index]);
        index += 1;
    }
    result
}

pub fn compare_bytes(left: &[u8], right: &[u8]) -> (result: i8)
    ensures
        (result == 0) == (left@ == right@),
        (result < 0) == bytes_lt(left@, right@),
        (result > 0) == bytes_lt(right@, left@),
{
    proof {
        lemma_bytes_lt_irrefl(left@);
        lemma_bytes_lt_asym(left@, right@);
        if left@ != right@ { lemma_bytes_lt_total(left@, right@); }
    }
    if bytes_lt_exec(left, right) { -1 }
    else if bytes_lt_exec(right, left) { 1 } else { 0 }
}

impl BagRow for BrassPartScan {
    type V = crate::gen_spec::PartRow;
    open spec fn rv(&self) -> Self::V { brass_part_scan_view(*self) }
    fn same(&self, other: &Self) -> (result: bool)
        ensures result == (self.rv() == other.rv()),
    {
        self.p_partkey == other.p_partkey && self.p_size == other.p_size &&
            compare_bytes(&self.p_mfgr, &other.p_mfgr) == 0 && compare_bytes(&self.p_type, &other.p_type) == 0
    }
    fn dup(&self) -> (result: Self)
        ensures result.rv() == self.rv(),
    {
        BrassPartScan { p_partkey: self.p_partkey, p_size: self.p_size, p_mfgr: copy_bytes(&self.p_mfgr), p_type: copy_bytes(&self.p_type) }
    }
}

impl BagRow for BrassPartsuppScan {
    type V = crate::gen_spec::PartsuppRow;
    open spec fn rv(&self) -> Self::V { brass_partsupp_scan_view(*self) }
    fn same(&self, other: &Self) -> (result: bool)
        ensures result == (self.rv() == other.rv()),
    {
        self.ps_partkey == other.ps_partkey && self.ps_suppkey == other.ps_suppkey && self.ps_supplycost == other.ps_supplycost
    }
    fn dup(&self) -> (result: Self)
        ensures result.rv() == self.rv(),
    { *self }
}

impl BagRow for SupplierScan {
    type V = crate::gen_spec::SupplierRow;
    open spec fn rv(&self) -> Self::V { supplier_scan_view(*self) }
    fn same(&self, other: &Self) -> (result: bool)
        ensures result == (self.rv() == other.rv()),
    {
        self.s_suppkey == other.s_suppkey && self.s_nationkey == other.s_nationkey && self.s_acctbal == other.s_acctbal &&
            compare_bytes(&self.s_name, &other.s_name) == 0 && compare_bytes(&self.s_address, &other.s_address) == 0 &&
            compare_bytes(&self.s_phone, &other.s_phone) == 0 && compare_bytes(&self.s_comment, &other.s_comment) == 0
    }
    fn dup(&self) -> (result: Self)
        ensures result.rv() == self.rv(),
    {
        SupplierScan { s_suppkey: self.s_suppkey, s_nationkey: self.s_nationkey, s_acctbal: self.s_acctbal,
            s_name: copy_bytes(&self.s_name), s_address: copy_bytes(&self.s_address),
            s_phone: copy_bytes(&self.s_phone), s_comment: copy_bytes(&self.s_comment) }
    }
}

impl BagRow for NationScan {
    type V = crate::gen_spec::NationRow;
    open spec fn rv(&self) -> Self::V { nation_scan_view(*self) }
    fn same(&self, other: &Self) -> (result: bool)
        ensures result == (self.rv() == other.rv()),
    {
        self.n_nationkey == other.n_nationkey && self.n_regionkey == other.n_regionkey && compare_bytes(&self.n_name, &other.n_name) == 0
    }
    fn dup(&self) -> (result: Self)
        ensures result.rv() == self.rv(),
    { NationScan { n_nationkey: self.n_nationkey, n_regionkey: self.n_regionkey, n_name: copy_bytes(&self.n_name) } }
}

pub struct Counted<Row> {
    pub row: Arc<Row>,
    pub count: i128,
}

pub struct Bag<Row> {
    pub entries: Vec<Counted<Row>>,
}

pub open spec fn weighted_entries<Row: BagRow>(entries: Seq<Counted<Row>>, weight: spec_fn(Row::V) -> int) -> int {
    sum(entries, |entry: Counted<Row>| entry.count as int * weight((*entry.row).rv()))
}

pub proof fn lemma_weighted_push<Row: BagRow>(entries: Seq<Counted<Row>>, entry: Counted<Row>, weight: spec_fn(Row::V) -> int)
    ensures weighted_entries(entries.push(entry), weight) == weighted_entries(entries, weight) + entry.count as int * weight((*entry.row).rv()),
{
    lemma_sum_push(entries, entry, |entry: Counted<Row>| entry.count as int * weight((*entry.row).rv()));
}

pub open spec fn magnitude(value: int) -> int {
    if value < 0 { -value } else { value }
}

pub open spec fn i128_max() -> int { 170141183460469231731687303715884105727 }

pub open spec fn entry_magnitude<Row: BagRow>(entry: Counted<Row>) -> int {
    magnitude(entry.count as int)
}

pub open spec fn entries_amount<Row: BagRow>(entries: Seq<Counted<Row>>, value: Row::V) -> int {
    sum(entries, |entry: Counted<Row>| if (*entry.row).rv() == value { entry.count as int } else { 0int })
}

pub open spec fn entries_mass<Row: BagRow>(entries: Seq<Counted<Row>>) -> int {
    sum(entries, |entry: Counted<Row>| entry_magnitude(entry))
}

impl<Row: BagRow> Bag<Row> {
    /// The sum of the magnitudes of the counts: no count is larger.
    pub open spec fn mass(self) -> int {
        entries_mass(self.entries@)
    }

    pub open spec fn weight(self, weight: spec_fn(Row::V) -> int) -> int {
        weighted_entries(self.entries@, weight)
    }

    pub open spec fn amount(self, row: Row::V) -> int {
        sum(self.entries@, |entry: Counted<Row>| if (*entry.row).rv() == row { entry.count as int } else { 0 })
    }

    pub open spec fn total(self) -> int {
        sum(self.entries@, |entry: Counted<Row>| entry.count as int)
    }

    pub open spec fn nonzero(self) -> bool {
        forall|index: int| 0 <= index < self.entries@.len() ==> (#[trigger] self.entries@[index]).count != 0
    }

    pub open spec fn unique(self) -> bool {
        forall|first: int, second: int| 0 <= first < second < self.entries@.len() ==>
            (*(#[trigger] self.entries@[first]).row).rv() != (*(#[trigger] self.entries@[second]).row).rv()
    }

    pub open spec fn valid(self) -> bool {
        &&& self.unique()
        &&& forall|index: int| 0 <= index < self.entries@.len() ==> (#[trigger] self.entries@[index]).count > 0
    }

    pub fn new() -> (result: Self)
        ensures result.entries@.len() == 0,
    { Bag { entries: Vec::new() } }

    pub fn adjust(&self, row: Arc<Row>, change: i128) -> (result: Option<Self>)
        ensures result is None ==> self.mass() + magnitude(change as int) > i128_max(),
            result is None ==> exists|k: int| 0 <= k < self.entries@.len() && (*self.entries@[k].row).rv() == (*row).rv() &&
                magnitude(self.entries@[k].count as int) + magnitude(change as int) > i128_max(),
            result matches Some(after) ==> after.mass() <= self.mass() + magnitude(change as int),
            result matches Some(after) ==> (self.nonzero() ==> after.nonzero()),
            result matches Some(after) ==> {
            &&& self.unique() ==> after.unique()
            &&& self.valid() && change > 0 ==> after.valid()
            &&& forall|index: int| 0 <= index < after.entries@.len() ==>
                (*((#[trigger] after.entries@[index]).row)).rv() == (*row).rv() ||
                exists|source: int| 0 <= source < self.entries@.len() &&
                    (*after.entries@[index].row).rv() == (*(#[trigger] self.entries@[source]).row).rv()
            &&& forall|weight: spec_fn(Row::V) -> int| #[trigger] after.weight(weight) == self.weight(weight) + change as int * weight((*row).rv())
            &&& forall|value: Row::V| #[trigger] after.amount(value) == self.amount(value) +
                if (*row).rv() == value { change as int } else { 0 }
        },
    {
        let mut entries: Vec<Counted<Row>> = Vec::new();
        let mut found = false;
        let mut index = 0;
        while index < self.entries.len()
            invariant
                index <= self.entries.len(),
                entries_mass(entries@) <= entries_mass(self.entries@.take(index as int)) + if found { magnitude(change as int) } else { 0int },
                forall|weight: spec_fn(Row::V) -> int|
                    #[trigger] weighted_entries(entries@, weight) ==
                    weighted_entries(self.entries@.take(index as int), weight) +
                    if found { change as int * weight((*row).rv()) } else { 0 },
                self.unique() ==> forall|first: int, second: int| 0 <= first < second < entries@.len() ==>
                    (*(#[trigger] entries@[first]).row).rv() != (*(#[trigger] entries@[second]).row).rv(),
                self.valid() && change > 0 ==> forall|offset: int| 0 <= offset < entries@.len() ==> (#[trigger] entries@[offset]).count > 0,
                forall|offset: int| 0 <= offset < entries@.len() ==>
                    exists|source: int| 0 <= source < index &&
                        (*(#[trigger] entries@[offset]).row).rv() == (*(#[trigger] self.entries@[source]).row).rv(),
                !found ==> forall|source: int| 0 <= source < index ==> (*(#[trigger] self.entries@[source]).row).rv() != (*row).rv(),
                self.nonzero() ==> forall|offset: int| 0 <= offset < entries@.len() ==> (#[trigger] entries@[offset]).count != 0,
            decreases self.entries.len() - index,
        {
            let ghost previous = entries@;
            let ghost previous_found = found;
            let entry = &self.entries[index];
            let ghost did_change = !found && (*entry.row).rv() == (*row).rv();
            let ghost future_count = if did_change { entry.count as int + change as int } else { entry.count as int };
            if !found && entry.row.same(&*row) {
                found = true;
                let count = match entry.count.checked_add(change) {
                    Some(value) => value,
                    None => {
                        proof {
                            lemma_sum_ge_term(self.entries@, |entry: Counted<Row>| entry_magnitude(entry), index as int);
                            assert(entry_magnitude(self.entries@[index as int]) == magnitude(entry.count as int));
                            assert(self.entries@[index as int] == *entry);
                            assert((*self.entries@[index as int].row).rv() == (*row).rv());
                        }
                        return None;
                    },
                };
                if count != 0 { entries.push(Counted { row: Arc::clone(&entry.row), count }); }
            } else { entries.push(Counted { row: Arc::clone(&entry.row), count: entry.count }); }
            proof {
                assert(found == (previous_found || did_change));
                lemma_sum_take(self.entries@, index as int, |entry: Counted<Row>| entry_magnitude(entry));
                if entries@.len() > previous.len() {
                    assert(entries@.drop_last() =~= previous);
                    lemma_sum_push(previous, entries@.last(), |entry: Counted<Row>| entry_magnitude(entry));
                    assert(entry_magnitude(entries@.last()) == magnitude(future_count));
                    assert(magnitude(future_count) <= magnitude(entry.count as int) + if did_change { magnitude(change as int) } else { 0int });
                } else {
                    assert(entries@ == previous);
                }
                assert(entries@.len() == previous.len() + if did_change && future_count == 0 { 0int } else { 1int });
                if entries@.len() > previous.len() {
                    assert(entries@.drop_last() =~= previous);
                    assert((*entries@.last().row).rv() == (*entry.row).rv());
                    assert(entries@.last().count as int == future_count);
                } else { assert(entries@ == previous); }
                assert forall|weight: spec_fn(Row::V) -> int|
                    #[trigger] weighted_entries(entries@, weight) ==
                    weighted_entries(self.entries@.take(index as int + 1), weight) +
                    if found { change as int * weight((*row).rv()) } else { 0 } by {
                    let summand = |entry: Counted<Row>| entry.count as int * weight((*entry.row).rv());
                    assert(weighted_entries(previous, weight) == weighted_entries(self.entries@.take(index as int), weight) +
                        if previous_found { change as int * weight((*row).rv()) } else { 0 });
                    lemma_sum_take(self.entries@, index as int, summand);
                    if entries@.len() > previous.len() {
                        assert(entries@.drop_last() =~= previous);
                        lemma_weighted_push(previous, entries@.last(), weight);
                    }
                    if found && !previous_found {
                        assert((entry.count as int + change as int) * weight((*row).rv()) ==
                            entry.count as int * weight((*row).rv()) + change as int * weight((*row).rv())) by (nonlinear_arith);
                    }
                }
                assert forall|offset: int| 0 <= offset < entries@.len() implies
                    exists|source: int| 0 <= source < index + 1 &&
                        (*(#[trigger] entries@[offset]).row).rv() == (*(#[trigger] self.entries@[source]).row).rv() by {
                    if offset < previous.len() {
                        let witness = choose|source: int| 0 <= source < index &&
                            (*previous[offset].row).rv() == (*(#[trigger] self.entries@[source]).row).rv();
                        assert((*entries@[offset].row).rv() == (*self.entries@[witness].row).rv());
                    } else { assert((*entries@[offset].row).rv() == (*self.entries@[index as int].row).rv()); }
                }
                if self.unique() {
                    assert forall|first: int, second: int| 0 <= first < second < entries@.len() implies
                        (*(#[trigger] entries@[first]).row).rv() != (*(#[trigger] entries@[second]).row).rv() by {
                        if second == previous.len() {
                            let witness = choose|source: int| 0 <= source < index &&
                                (*previous[first].row).rv() == (*(#[trigger] self.entries@[source]).row).rv();
                            assert((*entries@[first].row).rv() != (*self.entries@[index as int].row).rv());
                        } else { assert((*previous[first].row).rv() != (*previous[second].row).rv()); }
                    }
                }
            }
            index += 1;
        }
        let ghost previous = entries@;
        proof {
            assert(self.entries@.take(index as int) =~= self.entries@);
            assert forall|weight: spec_fn(Row::V) -> int| #[trigger] weighted_entries(previous, weight) == self.weight(weight) +
                if found { change as int * weight((*row).rv()) } else { 0 } by {}
        }
        if !found && change != 0 { entries.push(Counted { row: Arc::clone(&row), count: change }); }
        proof {
            assert forall|weight: spec_fn(Row::V) -> int|
                #[trigger] weighted_entries(entries@, weight) == self.weight(weight) + change as int * weight((*row).rv()) by {
                let summand = |entry: Counted<Row>| entry.count as int * weight((*entry.row).rv());
                assert(weighted_entries(previous, weight) == self.weight(weight) +
                    if found { change as int * weight((*row).rv()) } else { 0 });
                if !found && change != 0 {
                    assert(entries@ =~= previous.push(entries@.last()));
                    lemma_weighted_push(previous, entries@.last(), weight);
                }
            }
            if self.unique() && !found && change != 0 {
                assert forall|first: int, second: int| 0 <= first < second < entries@.len() implies
                    (*(#[trigger] entries@[first]).row).rv() != (*(#[trigger] entries@[second]).row).rv() by {
                    if second == previous.len() {
                        let witness = choose|source: int| 0 <= source < index &&
                            (*previous[first].row).rv() == (*(#[trigger] self.entries@[source]).row).rv();
                    } else { assert((*previous[first].row).rv() != (*previous[second].row).rv()); }
                }
            }
            assert forall|value: Row::V| #[trigger] (Bag { entries }).amount(value) == self.amount(value) +
                if (*row).rv() == value { change as int } else { 0 } by {
                let weight = |model: Row::V| if model == value { 1int } else { 0int };
                let summand = |entry: Counted<Row>| entry.count as int * weight((*entry.row).rv());
                let counter = |entry: Counted<Row>| if (*entry.row).rv() == value { entry.count as int } else { 0int };
                lemma_sum_ext_all(entries@, summand, counter);
                lemma_sum_ext_all(self.entries@, summand, counter);
                assert(weighted_entries(entries@, weight) == self.weight(weight) + change as int * weight((*row).rv()));
            }
        }
        proof {
            assert(self.entries@.take(index as int) =~= self.entries@);
            if !found && change != 0 {
                assert(entries@ =~= previous.push(entries@.last()));
                lemma_sum_push(previous, entries@.last(), |entry: Counted<Row>| entry_magnitude(entry));
                assert(entry_magnitude(entries@.last()) == magnitude(change as int));
            }
        }
        Some(Bag { entries })
    }
}

/// Weights that agree on the rows of the entries give the same total.
pub proof fn lemma_weighted_entries_ext<Row: BagRow>(entries: Seq<Counted<Row>>, first: spec_fn(Row::V) -> int, second: spec_fn(Row::V) -> int)
    requires forall|index: int| 0 <= index < entries.len() ==> first((*entries[index].row).rv()) == second((*entries[index].row).rv()),
    ensures weighted_entries(entries, first) == weighted_entries(entries, second),
{
    assert forall|index: int| 0 <= index < entries.len() implies
        (|entry: Counted<Row>| entry.count as int * first((*entry.row).rv()))(entries[index]) ==
        (|entry: Counted<Row>| entry.count as int * second((*entry.row).rv()))(entries[index]) by {}
    lemma_sum_ext(entries, |entry: Counted<Row>| entry.count as int * first((*entry.row).rv()),
        |entry: Counted<Row>| entry.count as int * second((*entry.row).rv()));
}

/// In a bag of distinct rows, a row's count is its amount.
pub proof fn lemma_entry_amount<Row: BagRow>(bag: Bag<Row>, k: int)
    requires bag.unique(), 0 <= k < bag.entries@.len(),
    ensures bag.amount((*bag.entries@[k].row).rv()) == bag.entries@[k].count as int,
{
    let value = (*bag.entries@[k].row).rv();
    let counter = |entry: Counted<Row>| if (*entry.row).rv() == value { entry.count as int } else { 0int };
    assert forall|other: int| 0 <= other < bag.entries@.len() && other != k implies counter(#[trigger] bag.entries@[other]) == 0 by {
        if other < k { assert((*bag.entries@[other].row).rv() != (*bag.entries@[k].row).rv()); }
        else { assert((*bag.entries@[k].row).rv() != (*bag.entries@[other].row).rv()); }
    }
    lemma_sum_single_match(bag.entries@, counter, k);
}

/// A sum of indicator weights is at most the number of summands.
pub proof fn lemma_indicator_sum_le<A>(rows: Seq<A>, weight: spec_fn(A) -> int)
    requires forall|row: A| 0 <= #[trigger] weight(row) <= 1,
    ensures 0 <= sum(rows, weight) <= rows.len(),
{
    lemma_sum_nonneg(rows, weight);
    lemma_sum_le(rows, weight, |row: A| 1int);
    verus_prooflib::lemma_sum_ones(rows);
}

pub fn capped_product(left: i128, right: i128) -> (result: i128)
    requires 0 <= left, 0 <= right,
    ensures
        0 <= result <= 100,
        result == if left as int * right as int >= 100 { 100int } else { left as int * right as int },
{
    if left == 0 || right == 0 { 0 }
    else if left >= 100 || right >= 100 {
        proof {
            assert((left as int) * (right as int) >= 100) by (nonlinear_arith)
                requires left > 0, right > 0, left >= 100 || right >= 100,
            {}
        }
        100
    }
    else {
        proof {
            assert(0 <= (left as int) * (right as int) < 10000) by (nonlinear_arith)
                requires 0 < left < 100, 0 < right < 100,
            {}
        }
        let product = left * right;
        if product >= 100 { 100 } else { product }
    }
}

}
