use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use verus_common::sums::sum;
use verus_prooflib::ordered::{Key, Index, Cursor, compare_bytes};
use crate::gen_spec::{OutRow, ext_lt};
use crate::gen_obligations::{CustomerInput, NationInput, customer_input_view, nation_input_view};
use crate::values::*;
use crate::facts::*;
use crate::paged::*;
use crate::ents::*;
use crate::customers::*;
use crate::dimensions::*;
use crate::calendar::*;
use crate::model::*;
use crate::gen_spec::{Db, query_result};
use verus_prooflib::wide::Wide;
use crate::gen_obligations::{out_view, params, answer_fits};

verus! {

pub type OutputRow = (i64, Vec<u8>, i128, i128, Vec<u8>, Vec<u8>, Vec<u8>, Vec<u8>);

pub struct Group {
    pub customer: Arc<CustomerInput>,
    pub nation: Arc<NationInput>,
    pub multiplicity: Wide,
}

pub struct Candidate {
    pub group: Arc<Group>,
    pub revenue: Wide,
}

impl Candidate {
    pub open spec fn view(self) -> OutRow {
        let customer = customer_input_view(*self.group.customer);
        let nation = nation_input_view(*self.group.nation);
        OutRow { c_custkey: customer.c_custkey, c_name: customer.c_name, revenue: self.revenue@,
            c_acctbal: customer.c_acctbal, c_phone: customer.c_phone, n_name: nation.n_name,
            c_address: customer.c_address, c_comment: customer.c_comment }
    }
}

pub open spec fn candidate_view_fn() -> spec_fn(Candidate) -> OutRow {
    |candidate: Candidate| candidate.view()
}

pub open spec fn candidate_rows(candidates: Seq<Candidate>) -> Seq<OutRow> {
    candidates.map_values(candidate_view_fn())
}

pub struct ReadSlice {
    pub slice: Arc<Slice>,
    pub cursor: PageCursor<RankEnt>,
    pub head: Option<RankEnt>,
    pub consumed: Ghost<int>,
}

pub open spec fn pending_rows(slice: ReadSlice) -> Seq<RankEnt> {
    if slice.head.is_some() { seq![slice.head.unwrap()] + slice.cursor.remaining() } else { slice.cursor.remaining() }
}

pub open spec fn head_upper(slice: ReadSlice) -> int {
    if slice.head.is_some() { slice.head.unwrap().upper as int } else { 0int }
}

/// The head's upper bound is exact: it is below the saturation point.
pub open spec fn exact_head(slice: ReadSlice) -> bool {
    slice.head.is_some() ==> slice.head.unwrap().upper < i64::MAX
}

pub open spec fn heads_exact(slices: Seq<ReadSlice>) -> bool {
    forall|position: int| 0 <= position < slices.len() ==> #[trigger] exact_head(slices[position])
}

pub open spec fn head_upper_weight() -> spec_fn(ReadSlice) -> int {
    |slice: ReadSlice| head_upper(slice)
}

pub open spec fn read_cell_index(slice: ReadSlice) -> Paged<CellEnt> {
    slice.slice.cells
}

pub open spec fn read_slice_valid(slice: ReadSlice) -> bool {
    &&& slice.slice.valid()
    &&& slice.cursor.well_formed()
    &&& 0 <= slice.consumed@ <= slice.slice.ranks.entries().len()
    &&& pending_rows(slice) == slice.slice.ranks.entries().skip(slice.consumed@)
    &&& slice.head.is_some() == (slice.consumed@ < slice.slice.ranks.entries().len())
}

pub proof fn lemma_head_cell(slice: ReadSlice)
    requires read_slice_valid(slice), slice.head.is_some(),
    ensures slice.head.unwrap().upper >= 0,
        sat64(cell_at(slice.slice.cells, slice.head.unwrap().customer as int).upper) == slice.head.unwrap().upper as int,
        slice.slice.ranks.entries()[slice.consumed@] == slice.head.unwrap(),
{
    let ranks = slice.slice.ranks;
    ranks.model();
    let head = slice.head.unwrap();
    assert(pending_rows(slice)[0] == head);
    assert(ranks.entries().skip(slice.consumed@)[0] == ranks.entries()[slice.consumed@]);
    assert(ranks.entries().contains(head));
    lemma_at(ranks, head.kv(), head);
    assert(ranks.at(head.kv()) == Some(head));
}

pub open spec fn read_slices_valid(slices: Seq<ReadSlice>) -> bool {
    forall|position: int| 0 <= position < slices.len() ==> #[trigger] read_slice_valid(slices[position])
}

pub open spec fn fresh_slices(slices: Seq<ReadSlice>) -> bool {
    forall|position: int| #![trigger slices[position]] 0 <= position < slices.len() ==> slices[position].consumed@ == 0
}

pub open spec fn slice_work(slice: ReadSlice) -> int {
    slice.cursor.remaining().len() as int + if slice.head.is_some() { 1int } else { 0int }
}

pub open spec fn remaining_work(slices: Seq<ReadSlice>) -> int {
    sum(slices, |slice: ReadSlice| slice_work(slice))
}

pub open spec fn read_moment_weight(customer: int, component: int) -> spec_fn(ReadSlice) -> int {
    |slice: ReadSlice| cell_at(slice.slice.cells, customer).component(component)
}

pub open spec fn read_moment_sum(slices: Seq<ReadSlice>, customer: int, component: int) -> int {
    sum(slices, read_moment_weight(customer, component))
}

pub fn add_range(result: &mut Vec<ReadSlice>, slices: &Index<Slice>, lower: i64, upper: i64)
    requires read_slices_valid(old(result)@), fresh_slices(old(result)@), slices_valid(*slices),
    ensures read_slices_valid(final(result)@), fresh_slices(final(result)@),
        forall|customer: int, component: int| #[trigger] read_moment_sum(final(result)@, customer, component)
            == read_moment_sum(old(result)@, customer, component)
                + sum(slices.entries(), crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)),
{
    let ghost original = result@;
    let ghost rows = slices.entries();
    if lower >= upper {
        proof {
            assert(slices_valid(*slices));
            assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
                == read_moment_sum(original, customer, component)
                    + sum(rows, crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)) by {
                verus_common::sums::lemma_sum_zero(rows,
                    crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component));
            }
        }
        return;
    }
    let snapshot = slices.share();
    proof {
        slices.model();
        verus_prooflib::ordered::lemma_lower_suffix(slices.entries(), (lower as int, Seq::<u8>::empty()));
    }
    let mut cursor = slices.cursor_from(&Key::number(lower));
    let ghost mut processed = verus_prooflib::ordered::lower_position(rows, (lower as int, Seq::<u8>::empty()));
    proof {
        assert forall|position: int| #![trigger rows[position]] 0 <= position < rows.len()
            implies rows[position].0.1 == Seq::<u8>::empty() by {
            assert(slices.at(rows[position].0).is_some());
        }
        assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
            == read_moment_sum(original, customer, component)
                + sum(rows.take(processed), crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)) by {
            let weight = crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component);
            assert forall|position: int| #![trigger rows.take(processed)[position]]
                0 <= position < processed implies weight(rows.take(processed)[position]) == 0 by {
                assert(rows.take(processed)[position] == rows[position]);
                assert(verus_prooflib::ordered::key_lt(rows[position].0, (lower as int, Seq::<u8>::empty())));
            }
            verus_common::sums::lemma_sum_zero(rows.take(processed), weight);
        }
        assert forall|position: int| #![trigger cursor.remaining()[position]]
            0 <= position < cursor.remaining().len() implies
                slices.at(cursor.remaining()[position].0) == Some(cursor.remaining()[position].1) by {
            let start = verus_prooflib::ordered::lower_position(slices.entries(), (lower as int, Seq::<u8>::empty()));
            assert(cursor.remaining()[position] == slices.entries()[start + position]);
        }
    }
    loop
        invariant read_slices_valid(result@), fresh_slices(result@), slices_valid(*slices),
            original == old(result)@,
            rows == slices.entries(), 0 <= processed <= rows.len(), cursor.remaining() == rows.skip(processed),
            verus_prooflib::ordered::ordered_rows(rows),
            forall|position: int| #![trigger rows[position]] 0 <= position < rows.len()
                ==> rows[position].0.1 == Seq::<u8>::empty(),
            forall|position: int| #![trigger rows[position]] 0 <= position < rows.len()
                ==> slices.at(rows[position].0) == Some(rows[position].1),
            forall|position: int| #![trigger rows[position]] processed <= position < rows.len()
                ==> rows[position].0.0 >= lower,
            forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
                == read_moment_sum(original, customer, component)
                    + sum(rows.take(processed), crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)),
            forall|position: int| #![trigger cursor.remaining()[position]]
                0 <= position < cursor.remaining().len()
                    ==> slices.at(cursor.remaining()[position].0) == Some(cursor.remaining()[position].1),
        decreases cursor.remaining().len(),
    {
        let ghost before_remaining = cursor.remaining();
        let (key, slice) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(rows.take(processed) =~= rows); }
                return;
            },
        };
        proof { assert((key.view(), *slice) == rows[processed]); }
        if key.number >= upper {
            proof {
                assert forall|position: int| #![trigger rows[position]] processed <= position < rows.len()
                    implies rows[position].0.0 >= upper by {
                    if position > processed {
                        assert(verus_prooflib::ordered::key_lt(rows[processed].0, rows[position].0));
                    }
                }
                assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
                    == read_moment_sum(original, customer, component)
                        + sum(rows, crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)) by {
                    crate::read_proofs::lemma_range_tail_zero(rows, processed, lower as int, upper as int, customer, component);
                }
            }
            return;
        }
        proof {
            assert(slices.at(before_remaining[0].0) == Some(before_remaining[0].1));
            assert(slices.at(key.view()) == Some(*slice));
            assert(slices.at(key.view()).is_some());
            assert(slices.at(key.view()).unwrap().valid());
            assert(slice.valid());
        }
        proof { slice.ranks.model(); }
        let mut ranks = slice.ranks.cursor();
        let ghost rank_rows = ranks.remaining();
        let head = ranks.next();
        let read = ReadSlice { slice, cursor: ranks, head, consumed: Ghost(0int) };
        proof {
            assert(rank_rows == slice.ranks.entries());
            assert(pending_rows(read) =~= rank_rows);
            if read.head.is_some() { assert(rank_rows.len() > 0); }
            assert(read.slice.valid());
            assert(0 <= read.consumed@ <= read.slice.ranks.entries().len());
            assert(read.head.is_some() == (read.consumed@ < read.slice.ranks.entries().len()));
            assert(pending_rows(read) == read.slice.ranks.entries().skip(read.consumed@));
            assert(read_slice_valid(read));
        }
        let ghost before_result = result@;
        result.push(read);
        proof {
            assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
                == read_moment_sum(original, customer, component)
                    + sum(rows.take(processed + 1), crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)) by {
                let weight = crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component);
                assert(read_moment_sum(before_result, customer, component)
                    == read_moment_sum(original, customer, component) + sum(rows.take(processed), weight));
                verus_common::sums::lemma_sum_push(before_result, read, read_moment_weight(customer, component));
                verus_common::sums::lemma_sum_take(rows, processed, weight);
                assert(weight(rows[processed]) == cell_at(read.slice.cells, customer).component(component));
                assert(read_moment_sum(result@, customer, component)
                    == read_moment_sum(before_result, customer, component) + cell_at(read.slice.cells, customer).component(component));
            }
            processed = processed + 1;
        }
    }
}

pub fn add_fact_range(result: &mut Vec<ReadSlice>, slices: &Index<Slice>, lower: i64, upper: i64,
    Ghost(db): Ghost<Db>, Ghost(grain): Ghost<int>)
    requires read_slices_valid(old(result)@), fresh_slices(old(result)@), slices_valid(*slices), facts_valid(db, *slices, grain),
    ensures read_slices_valid(final(result)@), fresh_slices(final(result)@),
        forall|customer: int, component: int| #[trigger] read_moment_sum(final(result)@, customer, component)
            == read_moment_sum(old(result)@, customer, component)
                + crate::read_proofs::fact_range(db, grain, lower as int, upper as int, customer, component),
{
    let ghost original = result@;
    add_range(result, slices, lower, upper);
    let snapshot = slices.share();
    proof {
        slices.model();
        assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
            == read_moment_sum(original, customer, component)
                + crate::read_proofs::fact_range(db, grain, lower as int, upper as int, customer, component) by {
            crate::read_proofs::lemma_range_facts(db, slices, grain, lower as int, upper as int, customer, component);
            assert(read_moment_sum(result@, customer, component) == read_moment_sum(original, customer, component)
                + sum(slices.entries(), crate::read_proofs::slice_range_weight(lower as int, upper as int, customer, component)));
        }
    }
}

pub fn add_three_months(result: &mut Vec<ReadSlice>, periods: &Periods, first: i64, Ghost(db): Ghost<Db>)
    requires i32::MIN <= first <= i32::MAX, read_slices_valid(old(result)@), fresh_slices(old(result)@),
        slices_valid(periods.quarters), slices_valid(periods.quarters1), slices_valid(periods.quarters2),
        facts_valid(db, periods.quarters, 2), facts_valid(db, periods.quarters1, 4), facts_valid(db, periods.quarters2, 5),
    ensures read_slices_valid(final(result)@), fresh_slices(final(result)@),
        forall|customer: int, component: int| #[trigger] read_moment_sum(final(result)@, customer, component)
            == read_moment_sum(old(result)@, customer, component)
                + crate::read_proofs::fact_range(db, 1, first as int, first as int + 3, customer, component),
{
    let remainder = first - floor_div(first, 3) * 3;
    let offset: i64 = if remainder == 0 { 0 } else if remainder == 1 { 2 } else { 1 };
    let quarter = floor_div(first + offset, 3);
    proof { assert(3 * quarter as int == first as int + offset as int); }
    if offset == 0 {
        add_fact_range(result, &periods.quarters, quarter, quarter + 1, Ghost(db), Ghost(2int));
    } else if offset == 1 {
        add_fact_range(result, &periods.quarters1, quarter, quarter + 1, Ghost(db), Ghost(4int));
    } else {
        add_fact_range(result, &periods.quarters2, quarter, quarter + 1, Ghost(db), Ghost(5int));
    }
    proof {
        assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
            == read_moment_sum(old(result)@, customer, component)
                + crate::read_proofs::fact_range(db, 1, first as int, first as int + 3, customer, component) by {
            crate::read_proofs::lemma_shifted_quarter(db, if offset == 0 { 2int } else if offset == 1 { 4int } else { 5int },
                first as int, quarter as int, customer, component);
        }
    }
}

#[verifier::rlimit(80)]
pub fn selected_slices(days: &Index<Slice>, periods: &Periods, lower: i32, upper: i32, Ghost(db): Ghost<Db>) -> (result: Vec<ReadSlice>)
    requires slices_valid(*days), slices_valid(periods.quarters),
        slices_valid(periods.quarters1), slices_valid(periods.quarters2),
        facts_valid(db, *days, 0), facts_valid(db, periods.quarters, 2),
        facts_valid(db, periods.quarters1, 4), facts_valid(db, periods.quarters2, 5),
    ensures read_slices_valid(result@), fresh_slices(result@),
        lower < upper ==> forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
            == crate::query_semantics::window_fact(db, params(lower as int, upper as int), customer, component),
{
    let mut result = Vec::new();
    if lower >= upper { return result; }
    let start = lower as i64;
    let end = upper as i64;
    let start_month = month_key(start);
    let end_month = month_key(end);
    if start_month + 3 == end_month && first_day(start_month) == start && first_day(end_month) == end {
        // three whole months (the shape of the TPC-H window): one slice of one of the three quarter grains
        proof { crate::calendar_proofs::lemma_month_start_order(start_month as int, end_month as int); }
        add_three_months(&mut result, periods, start_month, Ghost(db));
        proof {
            assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
                == crate::query_semantics::window_fact(db, params(lower as int, upper as int), customer, component) by {
                crate::read_proofs::lemma_month_range(db, start_month as int, end_month as int, customer, component);
                crate::read_proofs::lemma_window_range(db, params(lower as int, upper as int), customer, component);
            }
        }
        return result;
    }
    // otherwise: the whole quarters inside the window from the quarter grain, the rest day by day
    let first_month = if first_day(start_month) == start { start_month } else { start_month + 1 };
    let first_quarter = floor_div(first_month + 2, 3);
    let last_quarter = floor_div(end_month, 3);
    if first_month >= end_month || first_quarter >= last_quarter {
        add_fact_range(&mut result, days, start, end, Ghost(db), Ghost(0int));
    } else {
        proof {
            crate::calendar_proofs::lemma_day_cover_bounds(start as int, end as int);
            crate::calendar_proofs::lemma_month_start_order(first_month as int, first_quarter as int * 3);
            crate::calendar_proofs::lemma_month_start_order(first_quarter as int * 3, last_quarter as int * 3);
            crate::calendar_proofs::lemma_month_start_order(last_quarter as int * 3, end_month as int);
        }
        let first_date = first_day(first_quarter * 3);
        let last_date = first_day(last_quarter * 3);
        add_fact_range(&mut result, days, start, first_date, Ghost(db), Ghost(0int));
        let ghost first = result@;
        add_fact_range(&mut result, &periods.quarters, first_quarter, last_quarter, Ghost(db), Ghost(2int));
        let ghost middle = result@;
        add_fact_range(&mut result, days, last_date, end, Ghost(db), Ghost(0int));
        proof {
            assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
                == crate::read_proofs::fact_range(db, 0, start as int, end as int, customer, component) by {
                assert(read_moment_sum(first, customer, component)
                    == crate::read_proofs::fact_range(db, 0, start as int, first_date as int, customer, component));
                assert(read_moment_sum(middle, customer, component) == read_moment_sum(first, customer, component)
                    + crate::read_proofs::fact_range(db, 2, first_quarter as int, last_quarter as int, customer, component));
                assert(read_moment_sum(result@, customer, component) == read_moment_sum(middle, customer, component)
                    + crate::read_proofs::fact_range(db, 0, last_date as int, end as int, customer, component));
                crate::read_proofs::lemma_range_coarsen(db, 1, first_quarter as int, last_quarter as int, customer, component);
                crate::read_proofs::lemma_month_range(db, first_quarter as int * 3, last_quarter as int * 3, customer, component);
                crate::read_proofs::lemma_range_split(db, 0, start as int, first_date as int, end as int, customer, component);
                crate::read_proofs::lemma_range_split(db, 0, first_date as int, last_date as int, end as int, customer, component);
            }
        }
    }
    proof {
        assert forall|customer: int, component: int| #[trigger] read_moment_sum(result@, customer, component)
            == crate::query_semantics::window_fact(db, params(lower as int, upper as int), customer, component) by {
            crate::read_proofs::lemma_window_range(db, params(lower as int, upper as int), customer, component);
            assert(read_moment_sum(result@, customer, component)
                == crate::read_proofs::fact_range(db, 0, start as int, end as int, customer, component));
        }
    }
    result
}

/// An upper bound of the values (None: no bound is known, the largest rank is saturated).
pub fn maximum(index: &Index<Wide>) -> (result: Option<Wide>)
    requires ranked_values_valid(*index),
    ensures result matches Some(bound) ==> bound@ >= 0,
        result matches Some(bound) ==> bound@ == 0 ==> forall|key: (int, Seq<u8>)| #[trigger] index.at(key) == None,
        result matches Some(bound) ==> forall|key: (int, Seq<u8>)| #[trigger] index.at(key).is_some() ==> index.at(key).unwrap()@ <= bound@,
{
    let snapshot = index.share();
    proof { index.model(); }
    let mut cursor = index.cursor();
    match cursor.next() {
        Some((key, value)) => {
            proof { assert(index.at(key.view()) == Some(*value)); assert(value@ > 0); }
            if value.lt(&Wide::from_i128(i128::MAX)) {
                proof { crate::read_proofs::lemma_dimension_maximum(index, key.view(), value@); }
                Some(value.dup())
            } else { None }
        },
        None => Some(Wide::zero()),
    }
}

pub open spec fn group_key_view(customer: crate::gen_spec::CustomerRow, nation: crate::gen_spec::NationRow) -> (int, Seq<u8>) {
    (0, encode_text(customer.c_name) + encode_nat((customer.c_acctbal - i64::MIN as int) as nat, 8)
        + encode_text(customer.c_phone) + encode_text(nation.n_name)
        + encode_text(customer.c_address) + encode_text(customer.c_comment))
}

pub fn group_key(customer: &CustomerInput, nation: &NationInput) -> (result: Key)
    ensures result.view() == group_key_view(customer_input_view(*customer), nation_input_view(*nation)),
{
    let mut bytes = Vec::new();
    append_text(&mut bytes, customer.c_name.as_slice());
    append_i64(&mut bytes, customer.c_acctbal);
    append_text(&mut bytes, customer.c_phone.as_slice());
    append_text(&mut bytes, nation.n_name.as_slice());
    append_text(&mut bytes, customer.c_address.as_slice());
    append_text(&mut bytes, customer.c_comment.as_slice());
    Key::text(bytes)
}

pub fn add_group(result: &mut Index<Group>, cust: &Customer, nation: &Arc<Nation>, customer: i64)
    requires crate::group_proofs::groups_valid(*old(result), customer as int),
        cust.count@ > 0, nation.count@ > 0, cust.row.c_custkey == customer,
    ensures crate::group_proofs::groups_valid(*final(result), customer as int),
        forall|query: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(*final(result), query)
            == crate::group_proofs::group_total(*old(result), query)
                + if query == group_key_view(customer_input_view(*cust.row), nation_input_view(*nation.row)) {
                    cust.count@ * nation.count@
                } else { 0int },
{
    let ghost original = *result;
    let product = cust.count.mul(&nation.count);
    let key = group_key(&*cust.row, &*nation.row);
    let multiplicity = match result.get(&key) {
        Some(value) => value.multiplicity.add(&product),
        None => product.dup(),
    };
    proof {
        assert(product@ > 0) by (nonlinear_arith)
            requires product@ == cust.count@ * nation.count@, cust.count@ > 0, nation.count@ > 0;
        assert(multiplicity@ > 0);
        assert(multiplicity@ == crate::group_proofs::group_total(original, key.view()) + product@);
    }
    result.put(key, Arc::new(Group { customer: cust.row.clone(), nation: nation.row.clone(), multiplicity }));
    proof {
        assert forall|query: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(*result, query)
            == crate::group_proofs::group_total(original, query)
                + if query == group_key_view(customer_input_view(*cust.row), nation_input_view(*nation.row)) {
                    cust.count@ * nation.count@
                } else { 0int } by {}
        assert(crate::group_proofs::groups_valid(*result, customer as int));
    }
}

pub open spec fn nation_variant_rows(index: &Arc<Index<Nation>>) -> Seq<((int, Seq<u8>), Nation)> {
    index.entries()
}

pub open spec fn customer_variant_view(customer: &Customer) -> crate::gen_spec::CustomerRow {
    customer_input_view(*customer.row)
}

pub fn groups(dimensions: &Dimensions, customer: i64, Ghost(db): Ghost<Db>) -> (result: Index<Group>)
    requires dimensions_valid(db, *dimensions),
    ensures crate::group_proofs::groups_valid(result, customer as int),
        forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
            == crate::group_proofs::joined_group_sum(*dimensions, customer as int, key),
{
    let mut result: Index<Group> = Index::new();
    proof { lemma_dimensions_shape(db, *dimensions); reveal(dimensions_valid); }
    let probe = CustEnt { custkey: customer, variants: Arc::new(Vec::new()) };
    proof { assert(probe.kv() == (customer as int, Seq::<u8>::empty())); }
    let entry = match dimensions.customers.get(&probe) { Some(value) => value, None => return result };
    proof { assert(entry_valid(entry)); }
    let variants = &*entry.variants;
    let ghost customer_rows = variants@;
    proof { assert(customer_rows == variants_at(dimensions.customers, customer as int)); }
    let mut processed = 0usize;
    while processed < variants.len()
        invariant dimensions_valid(db, *dimensions), crate::group_proofs::groups_valid(result, customer as int),
            customer_rows == variants@, customer_rows == variants_at(dimensions.customers, customer as int),
            entry_valid(entry), variants == &*entry.variants,
            0 <= processed <= customer_rows.len(),
            forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
                == sum(customer_rows.take(processed as int), crate::group_proofs::customer_group_weight(*dimensions, key)),
        decreases customer_rows.len() - processed,
    {
        let cust = &variants[processed];
        proof {
            reveal(dimensions_valid);
            assert(cust.count@ > 0 && cust.row.c_custkey == customer);
        }
        let nations = match dimensions.nations.get(&Key::number(cust.row.c_nationkey)) {
            Some(value) => value,
            None => {
                proof {
                    assert forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
                        == sum(customer_rows.take(processed as int + 1), crate::group_proofs::customer_group_weight(*dimensions, key)) by {
                        let weight = crate::group_proofs::customer_group_weight(*dimensions, key);
                        verus_common::sums::lemma_sum_take(customer_rows, processed as int, weight);
                        assert(weight(customer_rows[processed as int]) == 0);
                        assert(crate::group_proofs::group_total(result, key) == sum(customer_rows.take(processed as int), weight));
                    }
                }
                processed += 1;
                continue;
            },
        };
        let ghost nation_rows = nation_variant_rows(&nations);
        let ghost before_nations = result;
        let ghost customer_row = customer_variant_view(cust);
        proof { nations.model(); }
        let mut names = nations.cursor();
        let ghost mut named = 0int;
        let mut finished = false;
        while !finished
            invariant dimensions_valid(db, *dimensions), crate::group_proofs::groups_valid(result, customer as int),
                0 <= processed < customer_rows.len(), customer_rows == variants@,
                customer_rows[processed as int] == *cust, cust.count@ > 0, cust.row.c_custkey == customer,
                customer_row == customer_input_view(*cust.row),
                dimensions.nations.at((customer_row.c_nationkey, Seq::empty())) == Some(*nations),
                nation_rows == nations.entries(), nations.wf(),
                forall|position: int| #![trigger nation_rows[position]] 0 <= position < nation_rows.len()
                    ==> nations.at(nation_rows[position].0) == Some(nation_rows[position].1),
                0 <= named <= nation_rows.len(), names.remaining() == nation_rows.skip(named),
                finished ==> named == nation_rows.len(),
                forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(before_nations, key)
                    == sum(customer_rows.take(processed as int), crate::group_proofs::customer_group_weight(*dimensions, key)),
                forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
                    == crate::group_proofs::group_total(before_nations, key)
                        + cust.count@ * sum(nation_rows.take(named), crate::group_proofs::nation_group_weight(customer_row, key)),
            decreases names.remaining().len() + if finished { 0nat } else { 1nat },
        {
            let (_, nation) = match names.next() {
                Some(row) => row,
                None => {
                    finished = true;
                    continue;
                },
            };
            proof {
                reveal(dimensions_valid);
                assert(nation_rows[named].1 == *nation);
                assert(dimensions.nations.at((customer_row.c_nationkey, Seq::empty())).is_some());
                assert(nations.at(nation_rows[named].0).is_some());
                assert(nations.at(nation_rows[named].0).unwrap().count@ > 0);
                assert(nation.count@ > 0);
            }
            let ghost original = result;
            add_group(&mut result, cust, &nation, customer);
            proof {
                assert forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
                    == crate::group_proofs::group_total(before_nations, key)
                        + cust.count@ * sum(nation_rows.take(named + 1), crate::group_proofs::nation_group_weight(customer_row, key)) by {
                    let weight = crate::group_proofs::nation_group_weight(customer_row, key);
                    verus_common::sums::lemma_sum_take(nation_rows, named, weight);
                    let previous = sum(nation_rows.take(named), weight);
                    let change = weight(nation_rows[named]);
                    assert(crate::group_proofs::group_total(original, key)
                        == crate::group_proofs::group_total(before_nations, key) + cust.count@ * previous);
                    assert(crate::group_proofs::group_total(result, key)
                        == crate::group_proofs::group_total(original, key) + cust.count@ * change);
                    assert(cust.count@ * previous + cust.count@ * change
                        == cust.count@ * (previous + change)) by (nonlinear_arith);
                }
                named = named + 1;
            }
        }
        proof {
            assert(nation_rows.take(named) =~= nation_rows);
            assert forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
                == sum(customer_rows.take(processed as int + 1), crate::group_proofs::customer_group_weight(*dimensions, key)) by {
                let weight = crate::group_proofs::customer_group_weight(*dimensions, key);
                verus_common::sums::lemma_sum_take(customer_rows, processed as int, weight);
                assert(weight(customer_rows[processed as int]) == cust.count@
                    * sum(nation_rows, crate::group_proofs::nation_group_weight(customer_row, key)));
                assert(crate::group_proofs::group_total(before_nations, key) == sum(customer_rows.take(processed as int), weight));
                assert(crate::group_proofs::group_total(result, key) == crate::group_proofs::group_total(before_nations, key)
                    + cust.count@ * sum(nation_rows, crate::group_proofs::nation_group_weight(customer_row, key)));
            }
        }
        processed += 1;
    }
    proof {
        assert(customer_rows.take(processed as int) =~= customer_rows);
        assert forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(result, key)
            == crate::group_proofs::joined_group_sum(*dimensions, customer as int, key) by {
            assert(crate::group_proofs::group_total(result, key)
                == sum(customer_rows, crate::group_proofs::customer_group_weight(*dimensions, key)));
        }
    }
    result
}

pub fn candidate_before(left: &Candidate, right: &Candidate) -> (result: bool)
    ensures result == ext_lt(left.view(), right.view()),
{
    if !left.revenue.eq(&right.revenue) { return right.revenue.lt(&left.revenue); }
    let left_customer = &*left.group.customer;
    let right_customer = &*right.group.customer;
    if left_customer.c_custkey != right_customer.c_custkey { return left_customer.c_custkey < right_customer.c_custkey; }
    let order = compare_bytes(left_customer.c_name.as_slice(), right_customer.c_name.as_slice());
    if order != 0 { return order < 0; }
    if left_customer.c_acctbal != right_customer.c_acctbal { return left_customer.c_acctbal < right_customer.c_acctbal; }
    let order = compare_bytes(left_customer.c_phone.as_slice(), right_customer.c_phone.as_slice());
    if order != 0 { return order < 0; }
    let order = compare_bytes(left.group.nation.n_name.as_slice(), right.group.nation.n_name.as_slice());
    if order != 0 { return order < 0; }
    let order = compare_bytes(left_customer.c_address.as_slice(), right_customer.c_address.as_slice());
    if order != 0 { return order < 0; }
    compare_bytes(left_customer.c_comment.as_slice(), right_customer.c_comment.as_slice()) < 0
}

pub fn insert_top(top: &mut Vec<Candidate>, candidate: Candidate)
    requires crate::selection::valid_top(candidate_rows(old(top)@)),
    ensures final(top).len() <= 20,
        !candidate_rows(old(top)@).contains(candidate.view()) ==> candidate_rows(final(top)@) == crate::gen_spec::limit_rows(
            verus_common::order::insert_by(candidate_rows(old(top)@), candidate.view(), crate::gen_spec::ext_order()), 20),
        candidate_rows(old(top)@).contains(candidate.view()) ==> candidate_rows(final(top)@) == candidate_rows(old(top)@),
        crate::selection::valid_top(candidate_rows(final(top)@)),
        crate::selection::covered(candidate_rows(final(top)@), candidate.view()),
        forall|row: OutRow| #[trigger] crate::selection::covered(candidate_rows(old(top)@), row)
            ==> crate::selection::covered(candidate_rows(final(top)@), row),
        forall|row: OutRow| #[trigger] candidate_rows(final(top)@).contains(row)
            ==> candidate_rows(old(top)@).contains(row) || row == candidate.view(),
        verus_common::order::nondesc(candidate_rows(old(top)@), crate::gen_spec::ext_order())
            ==> verus_common::order::nondesc(candidate_rows(final(top)@), crate::gen_spec::ext_order()),
{
    let ghost before = top@;
    let ghost rows = candidate_rows(before);
    let ghost inserted = candidate.view();
    let mut position = 0usize;
    let mut stopped = false;
    while position < top.len() && !stopped
        invariant position <= top.len(), top.len() <= 20, top@ == before,
            before == old(top)@, crate::selection::valid_top(rows),
            rows == candidate_rows(before), inserted == candidate.view(),
            forall|index: int| #![trigger rows[index]] 0 <= index < position ==> !ext_lt(inserted, rows[index]),
            stopped ==> position < top.len() && ext_lt(inserted, rows[position as int]),
        decreases top.len() - position + if stopped { 0int } else { 1int },
    {
        if candidate_before(&candidate, &top[position]) { stopped = true; }
        else { position += 1; }
    }
    if position > 0 && !candidate_before(&top[position - 1], &candidate) {
        proof {
            crate::selection::lemma_output_total(rows[position as int - 1], inserted);
            assert(rows[position as int - 1] == inserted);
            assert(rows.contains(inserted));
        }
        return;
    }
    proof {
        crate::selection::lemma_fresh_at(rows, inserted, position as int);
        crate::query_proofs::lemma_candidate_insert_position(rows, inserted, position as int);
        crate::selection::lemma_top_insert(rows, inserted, position as int);
    }
    if position < 20 {
        top.insert(position, candidate);
        proof { assert(candidate_rows(top@) =~= rows.insert(position as int, inserted)); }
        if top.len() > 20 { top.pop(); }
        proof {
            assert(candidate_rows(top@) =~= crate::gen_spec::limit_rows(rows.insert(position as int, inserted), 20));
        }
    } else {
        proof {
            assert(position == 20 && before.len() == 20);
            assert(rows.insert(position as int, inserted).take(20) =~= rows);
        }
    }
    proof {
        if verus_common::order::nondesc(rows, crate::gen_spec::ext_order()) {
            crate::query_proofs::lemma_candidate_bounded_order(rows, inserted);
        }
        crate::query_proofs::lemma_candidate_insert_support(rows, inserted);
        assert forall|row: OutRow| #[trigger] candidate_rows(top@).contains(row)
            implies rows.contains(row) || row == inserted by {
            assert(candidate_rows(top@) == crate::gen_spec::limit_rows(
                verus_common::order::insert_by(rows, inserted, crate::gen_spec::ext_order()), 20));
            crate::query_proofs::lemma_limit_support(verus_common::order::insert_by(rows, inserted, crate::gen_spec::ext_order()), row);
        }
    }
}

pub fn window_moments(slices: &Vec<ReadSlice>, customer: i64) -> (result: Moms)
    requires read_slices_valid(slices@),
    ensures forall|component: int| #[trigger] result@.component(component)
        == sum(slices@, |slice: ReadSlice| cell_at(slice.slice.cells, customer as int).component(component)),
        forall|component: int| #[trigger] result@.component(component)
            == read_moment_sum(slices@, customer as int, component),
{
    let mut result = Moms::zero();
    let probe = probe_cell(customer);
    let mut index = 0usize;
    while index < slices.len()
        invariant index <= slices.len(), read_slices_valid(slices@), probe.kv() == (customer as int, Seq::<u8>::empty()),
            forall|component: int| #[trigger] result@.component(component)
                == sum(slices@.take(index as int), |slice: ReadSlice|
                    cell_at(slice.slice.cells, customer as int).component(component)),
        decreases slices.len() - index,
    {
        let ghost processed = index as int;
        let ghost accumulated = result@;
        proof { assert(read_slice_valid(slices@[index as int])); assert(slices@[index as int].slice.cells.wf()); }
        let cell = match slices[index].slice.cells.get(&probe) {
            Some(value) => value.read(),
            None => Moms::zero(),
        };
        proof { assert(cell@ == cell_at(slices@[index as int].slice.cells, customer as int)); }
        result = result.add(&cell);
        proof {
            assert forall|component: int| #[trigger] result@.component(component)
                == sum(slices@.take(index as int + 1), |slice: ReadSlice|
                    cell_at(slice.slice.cells, customer as int).component(component)) by {
                assert(accumulated.component(component) == sum(slices@.take(index as int),
                    |slice: ReadSlice| cell_at(slice.slice.cells, customer as int).component(component)));
                assert(result@.component(component) == accumulated.component(component) + cell@.component(component));
                verus_common::sums::lemma_sum_take(slices@, index as int,
                    |slice: ReadSlice| cell_at(slice.slice.cells, customer as int).component(component));
            }
        }
        index += 1;
    }
    proof { assert(slices@.take(index as int) =~= slices@); }
    proof {
        assert forall|component: int| #[trigger] result@.component(component)
            == read_moment_sum(slices@, customer as int, component) by {
            assert(read_moment_weight(customer as int, component) =~=
                (|slice: ReadSlice| cell_at(slice.slice.cells, customer as int).component(component)));
        }
    }
    result
}

pub open spec fn top_rows_ok(db: Db, parameters: crate::gen_spec::Params, top: Seq<Candidate>) -> bool {
    forall|position: int| #![trigger top[position]] 0 <= position < top.len()
        ==> crate::gen_spec::row_ok(db, parameters, top[position].view())
}

pub fn insert_scored(top: &mut Vec<Candidate>, candidate: Candidate,
    Ghost(db): Ghost<Db>, Ghost(parameters): Ghost<crate::gen_spec::Params>)
    requires top.len() <= 20, top_rows_ok(db, parameters, old(top)@), crate::gen_spec::row_ok(db, parameters, candidate.view()),
        crate::selection::valid_top(candidate_rows(old(top)@)),
        verus_common::order::nondesc(candidate_rows(old(top)@), crate::gen_spec::ext_order()),
    ensures final(top).len() <= 20, top_rows_ok(db, parameters, final(top)@),
        crate::selection::valid_top(candidate_rows(final(top)@)),
        crate::selection::covered(candidate_rows(final(top)@), candidate.view()),
        forall|row: OutRow| #[trigger] crate::selection::covered(candidate_rows(old(top)@), row)
            ==> crate::selection::covered(candidate_rows(final(top)@), row),
        verus_common::order::nondesc(candidate_rows(final(top)@), crate::gen_spec::ext_order()),
{
    let ghost original = top@;
    let ghost row = candidate.view();
    insert_top(top, candidate);
    proof {
        assert forall|position: int| #![trigger top@[position]] 0 <= position < top.len()
            implies crate::gen_spec::row_ok(db, parameters, top@[position].view()) by {
            let value = top@[position].view();
            assert(candidate_rows(top@)[position] == value);
            assert(candidate_rows(top@).contains(value));
            assert(candidate_rows(original).contains(value) || value == row);
            if candidate_rows(original).contains(value) {
                let previous = choose|previous: int| #![trigger candidate_rows(original)[previous]]
                    0 <= previous < original.len() && candidate_rows(original)[previous] == value;
                assert(original[previous].view() == value);
            }
        }
    }
}

pub fn score(top: &mut Vec<Candidate>, dimensions: &Dimensions, slices: &Vec<ReadSlice>, customer: i64, hint: Moms,
    Ghost(db): Ghost<Db>, Ghost(parameters): Ghost<crate::gen_spec::Params>)
    requires top.len() <= 20, dimensions_valid(db, *dimensions), dimensions.nations.wf(),
        top_rows_ok(db, parameters, old(top)@),
        crate::selection::valid_top(candidate_rows(old(top)@)),
        verus_common::order::nondesc(candidate_rows(old(top)@), crate::gen_spec::ext_order()),
        read_moment_sum(slices@, customer as int, 0) == crate::query_semantics::window_fact(db, parameters, customer as int, 0),
        read_moment_sum(slices@, customer as int, 1) == crate::query_semantics::window_fact(db, parameters, customer as int, 1),
        slices.len() == 1 ==> hint@ == cell_at(slices@[0].slice.cells, customer as int),
        read_slices_valid(slices@),
    ensures final(top).len() <= 20, top_rows_ok(db, parameters, final(top)@),
        crate::selection::valid_top(candidate_rows(final(top)@)),
        forall|row: OutRow| #[trigger] crate::selection::covered(candidate_rows(old(top)@), row)
            ==> crate::selection::covered(candidate_rows(final(top)@), row),
        forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
            && row.c_custkey == customer as int ==> crate::selection::covered(candidate_rows(final(top)@), row),
        verus_common::order::nondesc(candidate_rows(final(top)@), crate::gen_spec::ext_order()),
{
    let ghost original_top = top@;
    let moments = if slices.len() == 1 { hint } else { window_moments(slices, customer) };
    proof {
        if slices.len() == 1 {
            assert(slices@ =~= seq![slices@[0]]);
            verus_common::sums::lemma_sum_singleton(slices@[0], read_moment_weight(customer as int, 0));
            verus_common::sums::lemma_sum_singleton(slices@[0], read_moment_weight(customer as int, 1));
        }
        assert(moments@.component(0) == read_moment_sum(slices@, customer as int, 0));
        assert(moments@.component(1) == read_moment_sum(slices@, customer as int, 1));
        assert(moments@.count == crate::query_semantics::window_fact(db, parameters, customer as int, 0));
        assert(moments@.revenue == crate::query_semantics::window_fact(db, parameters, customer as int, 1));
        reveal(dimensions_valid);
    }
    if !moments.count.is_pos() {
        proof {
            assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
                && row.c_custkey == customer as int implies false by {
                crate::query_semantics::lemma_row_factor(db, parameters, row);
            }
        }
        return;
    }
    let probe = CustEnt { custkey: customer, variants: Arc::new(Vec::new()) };
    proof { lemma_dimensions_shape(db, *dimensions); assert(probe.kv() == (customer as int, Seq::<u8>::empty())); }
    let entry = match dimensions.customers.get(&probe) { Some(value) => value, None => {
        proof {
            assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
                && row.c_custkey == customer as int implies false by {
                crate::completeness::lemma_representatives(db, parameters, *dimensions, row);
            }
        }
        return;
    } };
    proof { assert(entry_valid(entry)); }
    let variants = &*entry.variants;
    let ghost customer_rows = variants@;
    proof { assert(customer_rows == variants_at(dimensions.customers, customer as int)); }
    let cust = &variants[0];
    proof { assert(cust.count@ > 0 && cust.row.c_custkey == customer); }
    if variants.len() == 1 {
        let nations = match dimensions.nations.get(&Key::number(cust.row.c_nationkey)) { Some(value) => value, None => {
            proof {
                assert(customer_rows.len() == 1);
                assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
                    && row.c_custkey == customer as int implies false by {
                    let representatives = crate::completeness::lemma_representatives(db, parameters, *dimensions, row);
                    assert(customer_rows.contains(representatives.0));
                    assert(representatives.0 == *cust);
                }
            }
            return;
        } };
        let ghost nation_rows = nation_variant_rows(&nations);
        let mut nation_cursor = nations.cursor();
        let nation = match nation_cursor.next() { Some((_, value)) => value, None => {
            proof { assert(dimensions.nations.at((cust.row.c_nationkey as int, Seq::empty())).is_some()); }
            return;
        } };
        if nation_cursor.next().is_none() {
            proof {
                nations.model();
                assert(dimensions.nations.at((cust.row.c_nationkey as int, Seq::empty())).is_some());
                assert(nation_rows[0].1 == *nation);
                assert(nations.at(nation_rows[0].0).is_some());
                assert(nation.count@ > 0);
                assert(customer_rows.len() == 1 && nation_rows.len() == 1);
            }
            let multiplicity = cust.count.mul(&nation.count);
            let revenue = moments.revenue.mul(&multiplicity);
            let group = Arc::new(Group { customer: cust.row.clone(), nation: nation.row.clone(), multiplicity });
            let candidate = Candidate { group, revenue };
            proof {
                crate::group_proofs::lemma_singleton_candidate(db, parameters, *dimensions,
                    &*nations, cust, &*nation, candidate, moments@);
                assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
                    && row.c_custkey == customer as int implies row == candidate.view() by {
                    let representatives = crate::completeness::lemma_representatives(db, parameters, *dimensions, row);
                    assert(customer_rows.contains(representatives.0));
                    assert(representatives.0 == *cust);
                    assert(nation_rows.contains((nation_inner_view(nation_input_view(*representatives.1.row)), representatives.1)));
                    assert(representatives.1 == *nation);
                    crate::selection::lemma_equal_group_rows(db, parameters, row, candidate.view());
                }
            }
            insert_scored(top, candidate, Ghost(db), Ghost(parameters));
            return;
        }
    }
    let groups = groups(dimensions, customer, Ghost(db));
    proof { groups.model(); }
    let mut cursor = groups.cursor();
    proof {
        assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
            && row.c_custkey == customer as int implies exists|position: int| #![trigger cursor.remaining()[position]]
                0 <= position < cursor.remaining().len() && cursor.remaining()[position].0 == crate::group_proofs::output_group_key(row) by {
            crate::completeness::lemma_group_present(db, parameters, *dimensions, groups, customer, row);
        }
    }
    loop
        invariant top.len() <= 20, top_rows_ok(db, parameters, top@), dimensions_valid(db, *dimensions),
            original_top == old(top)@, crate::selection::valid_top(candidate_rows(top@)),
            forall|row: OutRow| #[trigger] crate::selection::covered(candidate_rows(original_top), row)
                ==> crate::selection::covered(candidate_rows(top@), row),
            verus_common::order::nondesc(candidate_rows(top@), crate::gen_spec::ext_order()),
            dimensions.nations.wf(),
            crate::group_proofs::groups_valid(groups, customer as int),
            forall|key: (int, Seq<u8>)| #[trigger] crate::group_proofs::group_total(groups, key)
                == crate::group_proofs::joined_group_sum(*dimensions, customer as int, key),
            moments@.count > 0, moments@.count == crate::query_semantics::window_fact(db, parameters, customer as int, 0),
            moments@.revenue == crate::query_semantics::window_fact(db, parameters, customer as int, 1),
            forall|position: int| #![trigger cursor.remaining()[position]] 0 <= position < cursor.remaining().len()
                ==> groups.at(cursor.remaining()[position].0) == Some(cursor.remaining()[position].1),
            forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
                && row.c_custkey == customer as int
                && !(exists|position: int| #![trigger cursor.remaining()[position]] 0 <= position < cursor.remaining().len()
                    && cursor.remaining()[position].0 == crate::group_proofs::output_group_key(row))
                ==> crate::selection::covered(candidate_rows(top@), row),
        decreases cursor.remaining().len(),
    {
        let ghost remaining = cursor.remaining();
        let ghost before_top = top@;
        let (_, group) = match cursor.next() { Some(row) => row, None => {
            proof { assert(remaining.len() == 0); }
            return;
        } };
        let revenue = moments.revenue.mul(&group.multiplicity);
        let candidate = Candidate { group, revenue };
        proof {
            assert(groups.at(remaining[0].0) == Some(*candidate.group));
            assert(groups.at(remaining[0].0).is_some());
            assert(remaining[0].0 == crate::group_proofs::output_group_key(candidate.view()));
            crate::group_proofs::lemma_coalesced_candidate(db, parameters, *dimensions, groups, candidate, customer, moments@);
        }
        insert_scored(top, candidate, Ghost(db), Ghost(parameters));
        proof {
            assert forall|row: OutRow| #[trigger] crate::selection::covered(candidate_rows(original_top), row)
                implies crate::selection::covered(candidate_rows(top@), row) by {}
            assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, parameters, row)
                && row.c_custkey == customer as int
                && !(exists|position: int| #![trigger cursor.remaining()[position]] 0 <= position < cursor.remaining().len()
                    && cursor.remaining()[position].0 == crate::group_proofs::output_group_key(row))
                implies crate::selection::covered(candidate_rows(top@), row) by {
                if remaining[0].0 == crate::group_proofs::output_group_key(row) {
                    crate::completeness::lemma_group_present(db, parameters, *dimensions, groups, customer, row);
                    crate::selection::lemma_equal_group_rows(db, parameters, row, candidate.view());
                } else {
                    assert(!(exists|position: int| #![trigger remaining[position]] 0 <= position < remaining.len()
                        && remaining[position].0 == crate::group_proofs::output_group_key(row))) by {
                        if exists|position: int| #![trigger remaining[position]] 0 <= position < remaining.len()
                            && remaining[position].0 == crate::group_proofs::output_group_key(row) {
                            let position = choose|position: int| #![trigger remaining[position]] 0 <= position < remaining.len()
                                && remaining[position].0 == crate::group_proofs::output_group_key(row);
                            assert(position > 0);
                            assert(cursor.remaining()[position - 1] == remaining[position]);
                        }
                    }
                    assert(crate::selection::covered(candidate_rows(before_top), row));
                }
            }
        }
    }
}

pub fn output_rows(top: &Vec<Candidate>) -> (result: Option<Vec<OutputRow>>)
    ensures result matches Some(rows) ==> rows.len() == top.len() && out_view(rows@) == candidate_rows(top@),
        result is None ==> exists|position: int| 0 <= position < top.len()
            && !(i128::MIN <= (#[trigger] top@[position]).revenue@ <= i128::MAX),
{
    let mut result = Vec::new();
    let mut index = 0usize;
    while index < top.len()
        invariant index <= top.len(), result.len() == index,
            out_view(result@) == candidate_rows(top@.take(index as int)),
        decreases top.len() - index,
    {
        let ghost processed = index as int;
        let candidate = &top[index];
        let customer = &*candidate.group.customer;
        let nation = &*candidate.group.nation;
        let revenue = match candidate.revenue.to_i128() { Some(value) => value, None => {
            proof { assert(!(i128::MIN <= top@[index as int].revenue@ <= i128::MAX)); }
            return None;
        } };
        result.push((customer.c_custkey, copy_bytes(customer.c_name.as_slice()), revenue,
            customer.c_acctbal as i128, copy_bytes(nation.n_name.as_slice()),
            copy_bytes(customer.c_address.as_slice()), copy_bytes(customer.c_phone.as_slice()),
            copy_bytes(customer.c_comment.as_slice())));
        proof {
            assert(candidate_rows(top@.take(index as int + 1)) =~=
                candidate_rows(top@.take(index as int)).push(candidate.view()));
        }
        index += 1;
    }
    proof { assert(top@.take(index as int) =~= top@); }
    Some(result)
}

/// The rows as the host receives them; declining proves the answer does not fit.
pub fn finish(top: &Vec<Candidate>, Ghost(db): Ghost<Db>, Ghost(parameters): Ghost<crate::gen_spec::Params>)
    -> (result: Option<Vec<OutputRow>>)
    requires query_result(db, parameters, candidate_rows(top@)),
    ensures result matches Some(out) ==> query_result(db, parameters, out_view(out@)),
        result is None ==> !answer_fits(db, parameters),
{
    let result = output_rows(top);
    proof {
        if result is None {
            let position = choose|position: int| 0 <= position < top.len()
                && !(i128::MIN <= (#[trigger] top@[position]).revenue@ <= i128::MAX);
            let rows = candidate_rows(top@);
            assert(rows[position].revenue == top@[position].revenue@);
            if answer_fits(db, parameters) {
                assert(crate::gen_obligations::out_fits(rows[position]));
            }
        }
    }
    result
}

/// The slice whose head has the highest upper bound, and the sum of the heads' upper bounds (None: not exact or too large).
pub fn scan_heads(slices: &Vec<ReadSlice>) -> (result: (Option<usize>, Option<i128>))
    requires read_slices_valid(slices@),
    ensures result.0 matches Some(position) ==> position < slices.len() && slices@[position as int].head.is_some(),
        result.0 is None ==> forall|position: int| #![trigger slices@[position]]
            0 <= position < slices.len() ==> slices@[position].head.is_none(),
        result.1 matches Some(total) ==> total as int == sum(slices@, head_upper_weight()) && heads_exact(slices@),
{
    let ghost loop_slices = slices@;
    let mut choice: Option<usize> = None;
    let mut highest = -1i128;
    let mut bound = Some(0i128);
    let mut index = 0usize;
        while index < slices.len()
            invariant index <= slices.len(),
                slices@ == loop_slices, read_slices_valid(slices@),
                choice matches Some(position) ==> position < index,
                choice matches Some(position) ==> slices@[position as int].head.is_some() && highest >= 0,
                choice.is_none() ==> highest == -1,
                choice.is_none() ==> forall|position: int| #![trigger slices@[position]]
                    0 <= position < index ==> slices@[position].head.is_none(),
                bound matches Some(total) ==> total as int == sum(slices@.take(index as int), head_upper_weight()),
                bound is Some ==> forall|position: int| 0 <= position < index ==> #[trigger] exact_head(slices@[position]),
            decreases slices.len() - index,
        {
            let ghost processed = index as int;
            proof { verus_common::sums::lemma_sum_take(slices@, processed, head_upper_weight()); }
            match &slices[index].head {
                Some(value) => {
                    proof {
                        assert(read_slice_valid(slices@[index as int]));
                        lemma_head_cell(slices@[index as int]);
                    }
                    let upper = value.upper as i128;
                    if upper > highest { highest = upper; choice = Some(index); }
                    bound = match bound {
                        Some(total) => if value.upper < i64::MAX { total.checked_add(upper) } else { None },
                        None => None,
                    };
                },
                None => {},
            }
            index += 1;
        }
    proof { assert(slices@.take(index as int) =~= slices@); }
    (choice, bound)
}

#[verifier::spinoff_prover]
#[verifier::rlimit(100)]
pub fn run(days: &Index<Slice>, periods: &Periods, dimensions: &Dimensions, lower: i32, upper: i32,
    Ghost(db): Ghost<Db>) -> (result: Option<Vec<OutputRow>>)
    requires facts_valid(db, *days, 0),
        facts_valid(db, periods.quarters, 2),
        facts_valid(db, periods.quarters1, 4), facts_valid(db, periods.quarters2, 5),
        dimensions_valid(db, *dimensions),
    ensures result matches Some(out) ==> query_result(db, params(lower as int, upper as int), out_view(out@)),
        result is None ==> !answer_fits(db, params(lower as int, upper as int)),
{
    proof {
        lemma_facts_shape(db, *days, 0);
        lemma_facts_shape(db, periods.quarters, 2);
        lemma_facts_shape(db, periods.quarters1, 4);
        lemma_facts_shape(db, periods.quarters2, 5);
        lemma_dimensions_shape(db, *dimensions);
        reveal(dimensions_valid);
        lemma_maxima_ranked_values(dimensions.nation_totals, dimensions.nation_maxima);
    }
    let mut top: Vec<Candidate> = Vec::new();
    if lower >= upper {
        let empty = Vec::new();
        proof {
            crate::query_proofs::lemma_empty_query(db, params(lower as int, upper as int));
            assert(out_view(empty@) =~= Seq::<crate::gen_spec::OutRow>::empty());
        }
        return Some(empty);
    }
    let max_customers = &dimensions.customer_max;
    let max_nations = maximum(&dimensions.nation_maxima);
    let nations_empty = match &max_nations { Some(bound) => bound.is_zero(), None => false };
    if max_customers.is_zero() || nations_empty {
        let empty = Vec::new();
        proof {
            crate::query_proofs::lemma_empty_dimensions(db, *dimensions);
            crate::query_proofs::lemma_empty_query(db, params(lower as int, upper as int));
            assert(out_view(empty@) =~= Seq::<crate::gen_spec::OutRow>::empty());
        }
        return Some(empty);
    }
    let multiplier = match &max_nations { Some(bound) => Some(max_customers.mul(bound)), None => None };
    proof {
        crate::completeness::lemma_factor_from_bounds(db, *dimensions, crate::completeness::opt_view(max_nations),
            crate::completeness::opt_view(multiplier));
    }
    let mut slices = selected_slices(days, periods, lower, upper, Ghost(db));
    let nation_snapshot = dimensions.nations.share();
    let mut visited: HashMap<i64, ()> = HashMap::new();
    let ghost mut seen: Set<i64> = Set::empty();
    loop
        invariant top.len() <= 20, read_slices_valid(slices@),
            dimensions_valid(db, *dimensions),
            dimensions.nations.wf(), top_rows_ok(db, params(lower as int, upper as int), top@),
            verus_common::order::nondesc(candidate_rows(top@), crate::gen_spec::ext_order()),
            crate::selection::valid_top(candidate_rows(top@)),
            crate::completeness::seen_prefix(slices@, seen),
            crate::completeness::seen_covered(db, params(lower as int, upper as int), candidate_rows(top@), seen),
            forall|customer: i64| #[trigger] visited@.dom().contains(customer) ==> seen.contains(customer),
            crate::completeness::factor_ok(db, crate::completeness::opt_view(multiplier)),
            forall|customer: int, component: int| #[trigger] read_moment_sum(slices@, customer, component)
                == crate::query_semantics::window_fact(db, params(lower as int, upper as int), customer, component),
        decreases remaining_work(slices@),
    {
        let (choice, bound) = scan_heads(&slices);
        let position = match choice { Some(value) => value, None => {
            proof {
                crate::completeness::lemma_exhausted(db, params(lower as int, upper as int), *dimensions,
                    slices@, candidate_rows(top@), seen);
            }
            return finish(&top, Ghost(db), Ghost(params(lower as int, upper as int)));
        } };
        if top.len() == 20 {
            let unseen_bound = match (&bound, &multiplier) {
                (Some(total), Some(limit)) => Some(Wide::from_i128(*total).mul(limit)),
                _ => None,
            };
            match unseen_bound {
                Some(limit) => { if limit.lt(&top[19].revenue) {
                    proof {
                        crate::completeness::lemma_threshold_factor(db, params(lower as int, upper as int), *dimensions,
                            slices@, candidate_rows(top@), seen, crate::completeness::opt_view(multiplier));
                    }
                    return finish(&top, Ghost(db), Ghost(params(lower as int, upper as int)));
                } },
                None => {},
            }
        }
        let ghost before_slices = slices@;
        let ghost before_seen = seen;
        let ghost before_top = top@;
        proof { assert(read_slice_valid(before_slices[position as int])); }
        let mut selected = slices.remove(position);
        let ghost before_selected = selected;
        let ghost removed_slices = slices@;
        proof { verus_common::sums::lemma_sum_remove(before_slices, position as int, |slice: ReadSlice| slice_work(slice)); }
        let customer = match &selected.head { Some(value) => value.customer, None => {
            proof { assert(false); }
            return None;
        } };
        let hint = match selected.slice.cells.get(&probe_cell(customer)) {
            Some(value) => value.read(),
            None => Moms::zero(),
        };
        proof {
            lemma_head_cell(before_selected);
            assert(hint@ == cell_at(before_selected.slice.cells, customer as int));
            assert(before_selected.slice.ranks.entries()[before_selected.consumed@].customer == customer);
        }
        selected.head = selected.cursor.next();
        selected.consumed = Ghost(before_selected.consumed@ + 1);
        proof {
            assert(pending_rows(selected) =~= before_selected.cursor.remaining());
            assert(pending_rows(selected) =~= pending_rows(before_selected).drop_first());
            assert(selected.slice.ranks.entries().skip(selected.consumed@) =~=
                selected.slice.ranks.entries().skip(before_selected.consumed@).drop_first());
            assert(read_slice_valid(selected));
            assert(slice_work(selected) == slice_work(before_selected) - 1);
            verus_prooflib::lemma_sum_insert(removed_slices, position as int, selected, |slice: ReadSlice| slice_work(slice));
        }
        slices.insert(position, selected);
        proof { seen = seen.insert(customer); }
        proof {
            assert forall|slice_position: int, rank_position: int| #![trigger slices@[slice_position].slice.ranks.entries()[rank_position]]
                0 <= slice_position < slices.len() && 0 <= rank_position < slices@[slice_position].consumed@
                implies seen.contains(slices@[slice_position].slice.ranks.entries()[rank_position].customer) by {
                if slice_position == position as int {
                    assert(slices@[slice_position] == selected);
                    if rank_position < before_selected.consumed@ {
                        assert(before_seen.contains(before_slices[position as int].slice.ranks.entries()[rank_position].customer));
                    }
                } else {
                    assert(slices@[slice_position] == before_slices[slice_position]);
                    assert(before_seen.contains(before_slices[slice_position].slice.ranks.entries()[rank_position].customer));
                }
            }
            assert forall|customer: int, component: int| #[trigger] read_moment_sum(slices@, customer, component)
                == crate::query_semantics::window_fact(db, params(lower as int, upper as int), customer, component) by {
                let weight = read_moment_weight(customer, component);
                assert(read_moment_sum(before_slices, customer, component)
                    == crate::query_semantics::window_fact(db, params(lower as int, upper as int), customer, component));
                verus_common::sums::lemma_sum_remove(before_slices, position as int, weight);
                verus_prooflib::lemma_sum_insert(removed_slices, position as int, selected, weight);
                assert(weight(selected) == weight(before_selected));
            }
            assert(read_slices_valid(slices@));
            assert(remaining_work(slices@) == remaining_work(before_slices) - 1);
            verus_common::sums::lemma_sum_nonneg(slices@, |slice: ReadSlice| slice_work(slice));
        }
        if slices.len() == 1 || !visited.contains_key(&customer) {
            if slices.len() != 1 { visited.insert(customer, ()); }
            score(&mut top, dimensions, &slices, customer, hint, Ghost(db), Ghost(params(lower as int, upper as int)));
        }
        proof {
            assert forall|row: OutRow| #[trigger] crate::gen_spec::row_ok(db, params(lower as int, upper as int), row)
                && seen.contains(row.c_custkey as i64) implies crate::selection::covered(candidate_rows(top@), row) by {
                crate::completeness::lemma_representatives(db, params(lower as int, upper as int), *dimensions, row);
                if row.c_custkey != customer as int {
                    assert(before_seen.contains(row.c_custkey as i64));
                    assert(crate::selection::covered(candidate_rows(before_top), row));
                }
            }
        }
    }
}

}
