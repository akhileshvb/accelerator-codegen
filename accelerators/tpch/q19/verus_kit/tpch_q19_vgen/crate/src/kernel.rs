use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::bytes_eq_exec;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;
use crate::values::*;
use verus_prooflib::*;
use verus_prooflib::wide::Wide;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct State {
    pub(crate) db: Ghost<Db>,
    pub(crate) parts: Store<PartCell>,
    pub(crate) histogram: Store<HistCell>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& parts_valid(self.parts)
        &&& histogram_valid(self.histogram)
        &&& forall|key: int, class: u8, brand: Seq<u8>|
            #![trigger part_value(self.parts, key, class, brand)]
            part_value(self.parts, key, class, brand) == part_sum(self.db(), key, class, brand)
        &&& forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
            #![trigger hist_value(self.histogram, class, brand, quantity, revenue)]
            hist_value(self.histogram, class, brand, quantity, revenue) == hist_sum(self.db(), class, brand, quantity, revenue)
    }

    pub(crate) proof fn facts(tracked &self)
        ensures parts_valid(self.parts), histogram_valid(self.histogram),
            forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(self.parts, key, class, brand)]
                part_value(self.parts, key, class, brand) == part_sum(self.db(), key, class, brand),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_value(self.histogram, class, brand, quantity, revenue)]
                hist_value(self.histogram, class, brand, quantity, revenue) == hist_sum(self.db(), class, brand, quantity, revenue),
    { use_type_invariant(self); }
}

pub fn build(
    qualifying_parts: Vec<PartInput>,
    qualifying_lines: &[LineInput],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_qualifying_parts_ok(db, qualifying_parts@),
        scan_qualifying_lines_ok(db, qualifying_lines@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r is Some,
{
    proof { crate::database::lemma_parts_classified(db, qualifying_parts@); }
    let mut parts = Work::new();
    let mut index = 0usize;
    while index < qualifying_parts.len()
        invariant index <= qualifying_parts.len(), part_patch_valid(parts),
            forall|row: int| #![trigger qualifying_parts@[row]] 0 <= row < qualifying_parts.len() ==>
                classified(part_input_view(qualifying_parts@[row])),
            forall|key: int, class: u8, brand: Seq<u8>|
                #![trigger part_work_value(parts, key, class, brand)]
                part_work_value(parts, key, class, brand) == part_scan_prefix(qualifying_parts@, index as int, key, class, brand),
        decreases qualifying_parts.len() - index,
    {
        let ghost processed = index as int;
        let row = &qualifying_parts[index];
        let class = input_class(row);
        let brand = Arc::new(copy_bytes(&row.p_brand));
        patch_part(&mut parts, row.p_partkey, class, &brand, &Wide::from_i128(1));
        proof {
            assert forall|key: int, other_class: u8, other_brand: Seq<u8>|
                #![trigger part_work_value(parts, key, other_class, other_brand)]
                part_work_value(parts, key, other_class, other_brand)
                    == part_scan_prefix(qualifying_parts@, index as int + 1, key, other_class, other_brand)
            by {
                lemma_sum_take(qualifying_parts@, index as int, |row: PartInput| part_scan_weight(row, key, other_class, other_brand));
            }
        }
        index += 1;
    }
    let ghost part_work = parts;
    let parts = parts.freeze();
    proof {
        assert forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(parts, key, class, brand)]
            part_value(parts, key, class, brand) == part_sum(db, key, class, brand) by {
            assert(part_value(parts, key, class, brand) == part_work_value(part_work, key, class, brand));
            crate::database::lemma_part_scan(db, qualifying_parts@, key, class, brand);
        }
    }
    let mut histogram = Work::new();
    let mut index = 0usize;
    while index < qualifying_lines.len()
        invariant index <= qualifying_lines.len(), parts_valid(parts), hist_patch_valid(histogram),
            forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(parts, key, class, brand)]
                part_value(parts, key, class, brand) == part_sum(db, key, class, brand),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_work_value(histogram, class, brand, quantity, revenue)]
                hist_work_value(histogram, class, brand, quantity, revenue)
                    == line_scan_prefix(parts, qualifying_lines@, index as int, class, brand, quantity, revenue),
        decreases qualifying_lines.len() - index,
    {
        let ghost processed = index as int;
        let row = &qualifying_lines[index];
        let revenue = amount(row.l_extendedprice, row.l_discount);
        match parts.get(row.l_partkey as u64) {
            Some(cells) => {
                contribute(&mut histogram, &*cells, row.l_quantity, 1, &revenue);
            },
            None => {},
        }
        proof {
            assert forall|class: u8, brand: Seq<u8>, quantity: int, is_revenue: bool|
                #![trigger hist_work_value(histogram, class, brand, quantity, is_revenue)]
                hist_work_value(histogram, class, brand, quantity, is_revenue)
                    == line_scan_prefix(parts, qualifying_lines@, index as int + 1, class, brand, quantity, is_revenue)
            by {
                lemma_sum_take(qualifying_lines@, index as int, |row: LineInput| line_scan_weight(parts, row, class, brand, quantity, is_revenue));
            }
        }
        index += 1;
    }
    let ghost histogram_work = histogram;
    let histogram = histogram.freeze();
    proof {
        assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
            #![trigger hist_value(histogram, class, brand, quantity, revenue)]
            hist_value(histogram, class, brand, quantity, revenue) == hist_sum(db, class, brand, quantity, revenue) by {
            assert(hist_value(histogram, class, brand, quantity, revenue) == hist_work_value(histogram_work, class, brand, quantity, revenue));
            crate::database::lemma_line_scan(db, parts, qualifying_lines@, class, brand, quantity, revenue);
        }
    }
    Some(State { db: Ghost(db), parts, histogram })
}

fn read_window(state: &State, class: u8, brand: &[u8], parameter: i64) -> (result: (Wide, Wide))
    requires class < 3,
    ensures result.0@ == window_prefix(state.histogram, class, brand@, parameter as int, 1001, false)
        && result.1@ == window_prefix(state.histogram, class, brand@, parameter as int, 1001, true),
{
    proof { use_type_invariant(state); }
    let mut count = Wide::zero();
    let mut revenue = Wide::zero();
    let mut offset = 0u64;
    let brand_hash = hash_brand(brand);
    while offset <= 1000
        invariant offset <= 1001, class < 3, state.valid(),
            brand_hash == fingerprint(brand@), brand_hash < 1000000007,
            count@ == window_prefix(state.histogram, class, brand@, parameter as int, offset as int, false),
            revenue@ == window_prefix(state.histogram, class, brand@, parameter as int, offset as int, true),
        decreases 1001 - offset,
    {
        let ghost previous_count = count@;
        let ghost previous_revenue = revenue@;
        let wide = parameter as i128 + offset as i128;
        if wide >= i64::MIN as i128 && wide <= i64::MAX as i128 {
            let quantity = wide as i64;
            let key = hash_hist_from_brand(class, brand_hash, quantity);
            proof {
                assert(key == hist_key(class, brand@, quantity));
            }
            match state.histogram.peek(key) {
                Some(cells) => {
                    let mut index = 0usize;
                    while index < cells.len()
                        invariant index <= cells.len(),
                            (*cells)@ == state.histogram.lookup(key),
                            count@ == previous_count + hist_cells_value((*cells)@.take(index as int), class, brand@, quantity as int, false),
                            revenue@ == previous_revenue + hist_cells_value((*cells)@.take(index as int), class, brand@, quantity as int, true),
                        decreases cells.len() - index,
                    {
                        let cell = &cells[index];
                        if cell.class == class && cell.quantity == quantity && bytes_eq_exec(&*cell.brand, brand) {
                            count = count.add(&cell.count);
                            revenue = revenue.add(&cell.revenue);
                        }
                        proof {
                            lemma_sum_take((*cells)@, index as int, |entry: HistCell| hist_cell_weight(entry, class, brand@, quantity as int, false));
                            lemma_sum_take((*cells)@, index as int, |entry: HistCell| hist_cell_weight(entry, class, brand@, quantity as int, true));
                        }
                        index += 1;
                    }
                },
                None => {},
            }
        }
        proof {
            lemma_range_step(offset as int, |position: int| hist_value(state.histogram, class, brand@, parameter as int + position, false));
            lemma_range_step(offset as int, |position: int| hist_value(state.histogram, class, brand@, parameter as int + position, true));
        }
        offset += 1;
    }
    (count, revenue)
}

pub fn execute(
    s: &State,
    p1: &[u8],
    p2: i64,
    p3: &[u8],
    p4: i64,
    p5: &[u8],
    p6: i64,
) -> (r: Option<Vec<Option<i128>>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1@, p2 as int, p3@, p4 as int, p5@, p6 as int), out_view(out@)),
        answer_fits(s.db(), params(p1@, p2 as int, p3@, p4 as int, p5@, p6 as int)) ==> r is Some,
{
    proof { use_type_invariant(s); }
    let first = read_window(s, 0, p1, p2);
    let second = read_window(s, 1, p3, p4);
    let third = read_window(s, 2, p5, p6);
    let count = first.0.add(&second.0).add(&third.0);
    let revenue = first.1.add(&second.1).add(&third.1);
    proof {
        crate::semantic::lemma_execute_result(s.db(), s.histogram,
            params(p1@, p2 as int, p3@, p4 as int, p5@, p6 as int), count@, revenue@);
    }
    let present = !count.is_zero();
    let value = if present {
        match revenue.to_i128() {
            Some(value) => Some(value),
            None => {
                proof {
                    let model = seq![OutRow { revenue: Some(revenue@) }];
                    assert(query_result(s.db(), params(p1@, p2 as int, p3@, p4 as int, p5@, p6 as int), model));
                    assert(!out_fits(model[0]));
                }
                return None;
            },
        }
    } else { None };
    let mut out = Vec::new();
    out.push(value);
    proof {
        assert(out_view(out@) =~= seq![OutRow { revenue: if count@ == 0 { None } else { Some(revenue@) } }]);
    }
    Some(out)
}

pub open spec fn rescan_lines_for_parts_wanted(s: State, lineitem_delta: Seq<LineDelta>, part_delta: Seq<PartDelta>) -> Seq<i64> {
    wanted_keys(part_delta)
}

pub fn rescan_lines_for_parts_keys(
    s: &State,
    lineitem_delta: &[LineDelta],
    part_delta: &Vec<PartDelta>,
) -> (r: Vec<i64>)
    ensures r@ == rescan_lines_for_parts_wanted(*s, lineitem_delta@, part_delta@),
{
    crate::maintenance::rescan_keys(part_delta)
}

pub fn apply(
    s: &State,
    lineitem_delta: &[LineDelta],
    part_delta: &Vec<PartDelta>,
    lines_for_parts_keys: &[i64],
    lines_for_parts_rows: &[LineInput],
    Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        lineitem_delta_ok(lineitem_delta@, lineitem_full),
        part_delta_ok(part_delta@, part_full),
        capture_ok(s.db(), db2, lineitem_full, part_full),
        lines_for_parts_keys@ == rescan_lines_for_parts_wanted(*s, lineitem_delta@, part_delta@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r is Ok,
{
    let (parts, histogram) = crate::maintenance::update(s, lineitem_delta, part_delta, lines_for_parts_keys, lines_for_parts_rows,
        Ghost(lineitem_full), Ghost(part_full), Ghost(db2));
    Ok(State { db: Ghost(db2), parts, histogram })
}

} // verus!
