use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::{class_total, lemma_equal_classes, lemma_flat_map_weight};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::bags::*;
use crate::storage::*;
use crate::parts::*;
use crate::dict::{dec, enc};
use crate::table::{Table, Cell, Major, rows_valid, minor_of, cell_sum};
use crate::enumeration::*;
use crate::enumeration_proofs::*;
use crate::minimum_proofs::*;
use crate::relational::*;
use crate::result_proofs::*;
use crate::join_parts::*;
use crate::key_proofs::*;
use crate::query_transfer::*;
use crate::maintenance::*;
use crate::output::supported_costs;
use crate::semantics::lemma_eligible_exists;

verus! {

/// The part-row class that a cell of the (size, type) list stands for.
pub open spec fn cell_cnt(ps: Parts, size: i64, t: int, cell: Cell) -> PartCnt {
    PartCnt { row: PartRow { p_partkey: cell.first as int, p_mfgr: ps.dtext(dec(cell.second)), p_type: ps.dtext(t), p_size: size as int },
        count: cell.count }
}

pub open spec fn tl_rows(ps: Parts, size: i64, t: int) -> Seq<Cell> { ps.tl.rows((size, enc(t), 0)) }

pub open spec fn eq_weight(x: PartRow) -> spec_fn(PartRow) -> int {
    |part: PartRow| if part == x { 1int } else { 0int }
}

/// The number of database rows equal to `x`.
pub open spec fn row_count(db: Db, x: PartRow) -> int { sum(db.part, eq_weight(x)) }

/// Facts about the cells of a (size, type) list.
pub proof fn lemma_tl_cell_facts(db: Db, ps: Parts, size: i64, t: int, index: int)
    requires ps.meaning(db), 0 <= t < ps.dl(), 0 <= index < tl_rows(ps, size, t).len(),
        forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size),
    ensures ({
        let cell = tl_rows(ps, size, t)[index];
        &&& cell.count > 0
        &&& dec(cell.second) < ps.dl() && cell.third == 0
        &&& row_count(db, cell_cnt(ps, size, t, cell).row) == cell.count as int
        &&& fits_i64(cell_cnt(ps, size, t, cell).row.p_partkey)
    }),
{
    ps.tl.lemma_shaped_valid();
    let rows = tl_rows(ps, size, t);
    let cell = rows[index];
    let key = (size, enc(t), 0i64);
    let minor = minor_of(cell);
    crate::table::lemma_cell_lookup(rows, minor, index);
    assert(ps.tl.at(key, minor, true) == cell_sum(rows, minor, true)) by { reveal(Table::at); }
    assert(ps.tl.at(key, minor, false) == cell_sum(rows, minor, false)) by { reveal(Table::at); }
    assert(cell.volume == 0);
    assert(cell.count != 0);
    // the relation to the part table
    assert(ps.tl.at((size, enc(t), 0), (cell.first, cell.second, cell.third), false) ==
        if cell.third == 0 { ps.tp.at((cell.first, 0, 0), (size, cell.second, enc(t)), false) } else { 0int });
    assert(cell.third == 0);
    if !(dec(cell.second) < ps.dl()) { lemma_out_of_range_zero(ps, cell.first, size, cell.second, enc(t)); }
    assert(dec(cell.second) < ps.dl());
    lemma_dict_id(ps, dec(cell.second));
    lemma_dict_id(ps, t);
    let x = cell_cnt(ps, size, t, cell).row;
    assert(ps.amount(cell.first, size, x) == ps.tp.at((cell.first, 0, 0), (size, cell.second, enc(t)), false));
    assert(ps.amount(cell.first, size, x) == db_amount(db, cell.first, size, x));
    lemma_sum_ext_all(db.part, amount_weight(cell.first, size, x), eq_weight(x));
    lemma_sum_nonneg(db.part, eq_weight(x));
    assert(row_count(db, x) == cell.count as int);
    assert(cell.count > 0);
    // some database row equals x, so its key fits
    let witness = lemma_sum_nonzero_witness(db.part, eq_weight(x));
    assert(db.part[witness] == x);
    assert(fits_i64(db.part[witness].p_partkey)) by { }
}


#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_row_class(db: Db, ps: Parts, size: i64, t: int, y: PartRow,
    left_project: spec_fn(PartRow) -> PartRow, right_project: spec_fn(Cell) -> PartRow,
    left_count: spec_fn(PartRow) -> int, right_count: spec_fn(Cell) -> int)
    requires ps.meaning(db), 0 <= t < ps.dl(),
        forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size) && fits_i64(db.part[index].p_partkey),
        forall|row: PartRow| #[trigger] left_project(row) == row,
        forall|row: PartRow| #[trigger] left_count(row) == if row.p_size == size as int && row.p_type == ps.dtext(t) { 1int } else { 0int },
        forall|cell: Cell| #[trigger] right_project(cell) == cell_cnt(ps, size, t, cell).row,
        forall|cell: Cell| #[trigger] right_count(cell) == cell.count as int,
    ensures class_total(db.part, left_project, left_count, y) == class_total(tl_rows(ps, size, t), right_project, right_count, y),
{
    ps.tl.lemma_shaped_valid();
    let rows = tl_rows(ps, size, t);
    let source = |row: PartRow| if left_project(row) == y { left_count(row) } else { 0int };
    let right_source = |cell: Cell| if right_project(cell) == y { right_count(cell) } else { 0int };
    verus_prooflib::lemma_class_total_weight(db.part, left_project, left_count, y, source);
    verus_prooflib::lemma_class_total_weight(rows, right_project, right_count, y, right_source);
    assert forall|index: int| 0 <= index < rows.len() implies
        cell_cnt(ps, size, t, rows[index]).row.p_size == size as int && cell_cnt(ps, size, t, rows[index]).row.p_type == ps.dtext(t) by {}
    if !(y.p_size == size as int && y.p_type == ps.dtext(t)) {
        assert forall|index: int| 0 <= index < db.part.len() implies source(#[trigger] db.part[index]) == 0 by {}
        lemma_sum_zero(db.part, source);
        assert forall|index: int| 0 <= index < rows.len() implies right_source(#[trigger] rows[index]) == 0 by {}
        lemma_sum_zero(rows, right_source);
    } else if ps.dict_has(y.p_mfgr) && fits_i64(y.p_partkey) && fits_i64(y.p_size) {
        let key = y.p_partkey as i64;
        let mid = ps.dict_id(y.p_mfgr);
        assert(0 <= mid < ps.dl() && ps.dtext(mid) == y.p_mfgr);
        let minor = (key, enc(mid), 0i64);
        let tkey = (size, enc(t), 0i64);
        assert forall|index: int| 0 <= index < rows.len() implies
            right_source(#[trigger] rows[index]) == crate::table::cell_weight(rows[index], minor, false) by {
            let cell = rows[index];
            lemma_tl_cell_facts(db, ps, size, t, index);
            if right_project(cell) == y {
                lemma_dict_ids_eq_ps(ps, dec(cell.second), mid);
                assert(cell.first == key && cell.second == enc(mid) && cell.third == 0);
                assert(minor_of(cell) == minor);
            } else if minor_of(cell) == minor {
                lemma_id_key(ps, mid);
                assert(dec(cell.second) == mid);
                assert(right_project(cell) == y);
            }
        }
        lemma_sum_ext(rows, right_source, |cell: Cell| crate::table::cell_weight(cell, minor, false));
        assert(ps.tl.at(tkey, minor, false) == cell_sum(rows, minor, false)) by { reveal(Table::at); }
        assert(cell_sum(rows, minor, false) == sum(rows, |cell: Cell| crate::table::cell_weight(cell, minor, false))) by { reveal(cell_sum); }
        // the list cell count equals the part table count equals the database count
        assert(ps.tl.at((size, enc(t), 0), (key, enc(mid), 0), false) == ps.tp.at((key, 0, 0), (size, enc(mid), enc(t)), false));
        lemma_dict_id(ps, t);
        assert(ps.dict_has(y.p_type));
        assert(ps.dict_id(y.p_type) == t);
        assert(ps.amount(key, size, y) == ps.tp.at((key, 0, 0), (size, enc(mid), enc(t)), false));
        assert(ps.amount(key, size, y) == db_amount(db, key, size, y));
        lemma_sum_ext_all(db.part, amount_weight(key, size, y), eq_weight(y));
        assert forall|index: int| 0 <= index < db.part.len() implies source(#[trigger] db.part[index]) == eq_weight(y)(db.part[index]) by {}
        lemma_sum_ext(db.part, source, eq_weight(y));
    } else {
        assert forall|index: int| 0 <= index < db.part.len() implies source(#[trigger] db.part[index]) == 0 by {
            assert(ps.dict_has(db.part[index].p_mfgr));
            assert(fits_i64(db.part[index].p_partkey));
        }
        lemma_sum_zero(db.part, source);
        assert forall|index: int| 0 <= index < rows.len() implies right_source(#[trigger] rows[index]) == 0 by {
            lemma_tl_cell_facts(db, ps, size, t, index);
            let cell = rows[index];
            assert(ps.dict_has(ps.dtext(dec(cell.second))));
        }
        lemma_sum_zero(rows, right_source);
    }
}

pub proof fn lemma_dict_ids_eq_ps(ps: Parts, first: int, second: int)
    requires (*ps.dict).valid(), 0 <= first < ps.dl(), 0 <= second < ps.dl(), ps.dtext(first) == ps.dtext(second),
    ensures first == second,
{
    lemma_dict_text_eq(ps, first, second);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_row_weight(db: Db, ps: Parts, size: i64, t: int, f: spec_fn(PartRow) -> int)
    requires ps.meaning(db), 0 <= t < ps.dl(),
        forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size) && fits_i64(db.part[index].p_partkey),
    ensures sum(tl_rows(ps, size, t), |cell: Cell| cell.count as int * f(cell_cnt(ps, size, t, cell).row)) ==
        sum(db.part, |part: PartRow| if part.p_size == size as int && part.p_type == ps.dtext(t) { f(part) } else { 0int }),
{
    let rows = tl_rows(ps, size, t);
    let left_project = |row: PartRow| row;
    let right_project = |cell: Cell| cell_cnt(ps, size, t, cell).row;
    let left_count = |row: PartRow| if row.p_size == size as int && row.p_type == ps.dtext(t) { 1int } else { 0int };
    let right_count = |cell: Cell| cell.count as int;
    assert forall|y: PartRow| #[trigger] class_total(db.part, left_project, left_count, y) == class_total(rows, right_project, right_count, y) by {
        lemma_row_class(db, ps, size, t, y, left_project, right_project, left_count, right_count);
    }
    lemma_equal_classes(db.part, rows, left_project, right_project, left_count, right_count, f);
    lemma_sum_ext_all(db.part, |row: PartRow| left_count(row) * f(left_project(row)),
        |part: PartRow| if part.p_size == size as int && part.p_type == ps.dtext(t) { f(part) } else { 0int });
    lemma_sum_ext_all(rows, |cell: Cell| right_count(cell) * f(right_project(cell)),
        |cell: Cell| cell.count as int * f(cell_cnt(ps, size, t, cell).row));
}


/// The output of a part key for the given part entries.
pub(crate) open spec fn key_output_p(state: State, key: i64, pents: Seq<PartCnt>, regions: Directory<Count>) -> Seq<OutRow> {
    match state.quotes.lookup(key) {
        None => Seq::empty(),
        Some(quotes) => {
            let costs = supported_costs(*quotes, state.suppliers, state.nations, regions);
            if costs.len() == 0 { Seq::empty() }
            else { quote_outputs((*quotes).entries@, pents, min_of(costs) as i64, state.suppliers, state.nations, regions) }
        },
    }
}

pub proof fn lemma_part_fold_single(cnt: PartCnt, supplier: SupplierRow, nation: NationRow, weight: spec_fn(OutRow) -> int)
    ensures part_fold(seq![cnt], supplier, nation, weight) == cnt.count as int * weight(row_projection(cnt.row, supplier, nation)),
{
    let single = seq![cnt];
    assert(single =~= Seq::<PartCnt>::empty().push(cnt));
    lemma_sum_empty(Seq::<PartCnt>::empty(), part_fold_weight(supplier, nation, weight));
    lemma_sum_push(Seq::<PartCnt>::empty(), cnt, part_fold_weight(supplier, nation, weight));
}

/// The weight of the output of one list cell: the cell's count times the query weight of its part row.
#[verifier::auto_ext_equal(assert, assert_by, ensures)]
#[verifier::rlimit(60)]
pub(crate) proof fn lemma_cell_weight_none(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, cnt: PartCnt, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@,
        cnt.count > 0, cnt.row.p_size == size as int, like_match(cnt.row.p_type, pat), fits_i64(cnt.row.p_partkey),
        row_count(state.db(), cnt.row) == cnt.count as int,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
        state.quotes.lookup(cnt.row.p_partkey as i64) is None,
    ensures sum(key_output_p(state, cnt.row.p_partkey as i64, seq![cnt], regions), weight) ==
        cnt.count as int * part_query_weight(state.db(), params(size as int, pat, name), cnt.row, weight),
{
    let db = state.db();
    let p = params(size as int, pat, name);
    let x = cnt.row;
    let key = x.p_partkey as i64;
    assert(key as int == x.p_partkey);
    let parts = seq![cnt];
    assert(part_selected(x, p));
    let witness = lemma_sum_nonzero_witness(db.part, eq_weight(x));
    assert(db.part[witness] == x);
    lemma_eligible_exists(db, key);
    assert(eligible(db, key) > 0);
    lemma_regions_nonnegative(state, regions, name);
    let qj = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, x, quote) { weight(row_projection(x, supplier, nation)) } else { 0int };
        lemma_quote_weight(state, key, |quote: PartsuppRow| 1int);
        let indicator = |quote: PartsuppRow| if quote.ps_partkey == key as int { 1int } else { 0int };
        assert((|quote: PartsuppRow| if quote.ps_partkey == key as int { (|quote: PartsuppRow| 1int)(quote) } else { 0int }) =~= indicator);
        assert(sum(db.partsupp, indicator) == 0);
        assert forall|index: int| 0 <= index < db.partsupp.len() implies (#[trigger] db.partsupp[index]).ps_partkey != key as int by {
            if db.partsupp[index].ps_partkey == key as int { lemma_sum_pos(db.partsupp, indicator, index); }
        }
        lemma_quote_join_empty(db, name, key as int, qj);
        assert(part_query_weight(db, p, x, weight) == 0);
        assert(key_output_p(state, key, parts, regions).len() == 0);
        lemma_sum_empty(key_output_p(state, key, parts, regions), weight);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
#[verifier::rlimit(60)]
pub(crate) proof fn lemma_cell_weight_empty(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, cnt: PartCnt, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@,
        cnt.count > 0, cnt.row.p_size == size as int, like_match(cnt.row.p_type, pat), fits_i64(cnt.row.p_partkey),
        row_count(state.db(), cnt.row) == cnt.count as int,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
        state.quotes.lookup(cnt.row.p_partkey as i64) is Some, key_costs(state, regions, cnt.row.p_partkey as i64).len() == 0,
    ensures sum(key_output_p(state, cnt.row.p_partkey as i64, seq![cnt], regions), weight) ==
        cnt.count as int * part_query_weight(state.db(), params(size as int, pat, name), cnt.row, weight),
{
    let db = state.db();
    let p = params(size as int, pat, name);
    let x = cnt.row;
    let key = x.p_partkey as i64;
    assert(key as int == x.p_partkey);
    let parts = seq![cnt];
    assert(part_selected(x, p));
    let witness = lemma_sum_nonzero_witness(db.part, eq_weight(x));
    assert(db.part[witness] == x);
    lemma_eligible_exists(db, key);
    assert(eligible(db, key) > 0);
    lemma_regions_nonnegative(state, regions, name);
    let qj = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, x, quote) { weight(row_projection(x, supplier, nation)) } else { 0int };
        lemma_minimum_query(state, regions, name, pat, key, x);
        assert(qj =~= (|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| 0int));
        lemma_quote_join_zero(db, name, key as int);
        assert(part_query_weight(db, p, x, weight) == 0);
        assert(key_output_p(state, key, parts, regions).len() == 0);
        lemma_sum_empty(key_output_p(state, key, parts, regions), weight);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
#[verifier::rlimit(60)]
pub(crate) proof fn lemma_cell_weight_live(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, cnt: PartCnt, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@,
        cnt.count > 0, cnt.row.p_size == size as int, like_match(cnt.row.p_type, pat), fits_i64(cnt.row.p_partkey),
        row_count(state.db(), cnt.row) == cnt.count as int,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
        state.quotes.lookup(cnt.row.p_partkey as i64) is Some, key_costs(state, regions, cnt.row.p_partkey as i64).len() > 0,
    ensures sum(key_output_p(state, cnt.row.p_partkey as i64, seq![cnt], regions), weight) ==
        cnt.count as int * part_query_weight(state.db(), params(size as int, pat, name), cnt.row, weight),
{
    let db = state.db();
    let p = params(size as int, pat, name);
    let x = cnt.row;
    let key = x.p_partkey as i64;
    assert(key as int == x.p_partkey);
    let parts = seq![cnt];
    assert(part_selected(x, p));
    let witness = lemma_sum_nonzero_witness(db.part, eq_weight(x));
    assert(db.part[witness] == x);
    lemma_eligible_exists(db, key);
    assert(eligible(db, key) > 0);
    lemma_regions_nonnegative(state, regions, name);
    let qj = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, x, quote) { weight(row_projection(x, supplier, nation)) } else { 0int };
        let quotes_bag = state.quotes.lookup(key)->0;
        let costs = key_costs(state, regions, key);
        let cost = min_of(costs);
        lemma_cost_bounds(state, regions, key, costs.len() as int);
        assert(costs.take(costs.len() as int) =~= costs);
        let position = choose|position: int| 0 <= position < costs.len() && #[trigger] costs[position] == cost;
        let mapper = |quote: Counted<crate::gen_obligations::BrassPartsuppScan>| if quote.count > 0 && crate::output::supported(*quote.row, state.suppliers, state.nations, regions) { seq![(*quote.row).ps_supplycost as int] } else { Seq::<int>::empty() };
        lemma_costs_mapper(state, regions, key, mapper);
        verus_prooflib::lemma_flat_map_contains((*quotes_bag).entries@, mapper, cost);
        let quote_position = choose|index: int| 0 <= index < (*quotes_bag).entries@.len() && #[trigger] mapper((*quotes_bag).entries@[index]).contains(cost);
        assert(fits_i64(cost));
        assert forall|index: int| 0 <= index < parts.len() implies (#[trigger] parts[index]).count > 0 by {}
        lemma_quote_outputs_weight(state, key, parts, cost as i64, regions, weight);
        let indexed = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { part_fold(parts, supplier, nation, weight) } else { 0int };
        lemma_index_quote(state, regions, name, key, indexed);
        let keep = |part: PartRow| part == x;
        let row_weight = |part: PartRow, quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { weight(row_projection(part, supplier, nation)) } else { 0int };
        let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { row_weight(part, quote, supplier, nation) } else { 0int });
        assert forall|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| #[trigger] indexed(quote, supplier, nation) == joined(quote, supplier, nation) by {
            if quote.ps_supplycost == cost {
                lemma_part_fold_single(cnt, supplier, nation, weight);
                let value = weight(row_projection(x, supplier, nation));
                let single = |part: PartRow| if part == x { value } else { 0int };
                assert forall|part: PartRow| #[trigger] single(part) == value * eq_weight(x)(part) by {
                    if part == x { assert(value * 1 == value); } else { assert(value * 0 == 0int); }
                }
                lemma_sum_scale(db.part, eq_weight(x), single, value);
                lemma_sum_ext_all(db.part, |part: PartRow| if keep(part) { row_weight(part, quote, supplier, nation) } else { 0int }, single);
                assert(sum(db.part, single) == value * row_count(db, x));
                assert(value * (cnt.count as int) == (cnt.count as int) * value) by (nonlinear_arith);
            } else {
                lemma_sum_zero(db.part, |part: PartRow| if keep(part) { row_weight(part, quote, supplier, nation) } else { 0int });
            }
        }
        assert(indexed =~= joined);
        lemma_quote_parts(db, name, key as int, keep, row_weight);
        let normalized = |part: PartRow| if keep(part) { quote_join(db, name, key as int, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| row_weight(part, quote, supplier, nation)) } else { 0int };
        lemma_minimum_query(state, regions, name, pat, key, x);
        let left = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| row_weight(x, quote, supplier, nation);
        assert forall|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| quote.ps_partkey == key as int implies #[trigger] left(quote, supplier, nation) == qj(quote, supplier, nation) by {
            assert(scalar_1_value(db, Params { p1: x.p_size, p2: pat, p3: name }, x) == Some(cost));
            assert(quote_selected(db, p, x, quote) == (quote.ps_supplycost == cost));
        }
        lemma_quote_join_ext(db, name, key as int, left, qj);
        let pq = quote_join(db, name, key as int, qj);
        assert(part_query_weight(db, p, x, weight) == pq);
        let scaled = |part: PartRow| pq * eq_weight(x)(part);
        assert forall|part: PartRow| #[trigger] normalized(part) == scaled(part) by {
            if part == x {
                assert((|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| row_weight(part, quote, supplier, nation)) =~= left);
                assert(pq * 1 == pq);
            } else { assert(pq * 0 == 0int); }
        }
        lemma_sum_ext_all(db.part, normalized, scaled);
        lemma_sum_scale(db.part, eq_weight(x), scaled, pq);
        assert(sum(db.part, normalized) == pq * row_count(db, x));
        assert(key_output_p(state, key, parts, regions) == quote_outputs((*quotes_bag).entries@, parts, cost as i64, state.suppliers, state.nations, regions));
        assert(pq * (cnt.count as int) == (cnt.count as int) * pq) by (nonlinear_arith);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
#[verifier::rlimit(60)]
pub(crate) proof fn lemma_cell_weight(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, cnt: PartCnt, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@,
        cnt.count > 0, cnt.row.p_size == size as int, like_match(cnt.row.p_type, pat), fits_i64(cnt.row.p_partkey),
        row_count(state.db(), cnt.row) == cnt.count as int,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures sum(key_output_p(state, cnt.row.p_partkey as i64, seq![cnt], regions), weight) ==
        cnt.count as int * part_query_weight(state.db(), params(size as int, pat, name), cnt.row, weight),
{
    let key = cnt.row.p_partkey as i64;
    if state.quotes.lookup(key) is None { lemma_cell_weight_none(state, size, name, pat, cnt, regions, weight); }
    else if key_costs(state, regions, key).len() == 0 { lemma_cell_weight_empty(state, size, name, pat, cnt, regions, weight); }
    else { lemma_cell_weight_live(state, size, name, pat, cnt, regions, weight); }
}

pub open spec fn ids(n: int) -> Seq<int> { Seq::new(n as nat, |i: int| i) }

pub open spec fn group_weight(ps: Parts, f: spec_fn(PartRow) -> int, t: int) -> spec_fn(PartRow) -> int {
    |part: PartRow| if (|part: PartRow| if ps.dict_has(part.p_type) { ps.dict_id(part.p_type) } else { -1int })(part) == t { f(part) } else { 0int }
}

pub open spec fn type_key(ps: Parts) -> spec_fn(PartRow) -> int {
    |part: PartRow| if ps.dict_has(part.p_type) { ps.dict_id(part.p_type) } else { -1int }
}

/// Sum over database rows, grouped by the type's dictionary id.
#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_types_group(db: Db, ps: Parts, size: i64, pat: Seq<u8>, f: spec_fn(PartRow) -> int)
    requires ps.meaning(db),
        forall|part: PartRow| f(part) != 0 ==> part.p_size == size as int && like_match(part.p_type, pat),
    ensures sum(db.part, f) == sum(ids(ps.dl()), |t: int| if like_match(ps.dtext(t), pat) {
        sum(db.part, |part: PartRow| if part.p_size == size as int && part.p_type == ps.dtext(t) { f(part) } else { 0int })
    } else { 0int }),
{
    let dl = ps.dl();
    let ks = ids(dl);
    let per = |t: int| sum(db.part, group_weight(ps, f, t));
    assert(verus_common::sums::distinct(ks)) by {
        assert forall|i: int, j: int| 0 <= i < ks.len() && 0 <= j < ks.len() && i != j implies ks[i] != ks[j] by {}
    }
    assert forall|i: int| 0 <= i < db.part.len() && f(db.part[i]) != 0 implies ks.contains(type_key(ps)(#[trigger] db.part[i])) by {
        let part = db.part[i];
        assert(ps.dict_has(part.p_type));
        let id = ps.dict_id(part.p_type);
        assert(0 <= id < dl);
        assert(ks[id] == id);
    }
    assert forall|k: int| #[trigger] per(k) == sum(db.part, |x: PartRow| if type_key(ps)(x) == k { f(x) } else { 0int }) by {
        lemma_sum_ext_all(db.part, group_weight(ps, f, k), |x: PartRow| if type_key(ps)(x) == k { f(x) } else { 0int });
    }
    lemma_sum_group(db.part, ks, type_key(ps), f, per);
    let target = |t: int| if like_match(ps.dtext(t), pat) {
        sum(db.part, |part: PartRow| if part.p_size == size as int && part.p_type == ps.dtext(t) { f(part) } else { 0int })
    } else { 0int };
    assert forall|i: int| 0 <= i < ks.len() implies per(#[trigger] ks[i]) == target(ks[i]) by {
        let t = ks[i];
        assert(0 <= t < dl);
        lemma_dict_id(ps, t);
        let lhs = |part: PartRow| if type_key(ps)(part) == t { f(part) } else { 0int };
        let rhs = |part: PartRow| if part.p_size == size as int && part.p_type == ps.dtext(t) { f(part) } else { 0int };
        assert(per(t) == sum(db.part, group_weight(ps, f, t)));
        lemma_sum_ext_all(db.part, group_weight(ps, f, t), lhs);
        assert(per(t) == sum(db.part, lhs));
        assert(like_match(ps.dtext(t), pat) ==> target(t) == sum(db.part, rhs));
        assert(!like_match(ps.dtext(t), pat) ==> target(t) == 0);
        if like_match(ps.dtext(t), pat) {
            assert forall|part: PartRow| #[trigger] lhs(part) == rhs(part) by {
                if ps.dict_has(part.p_type) {
                    let id = ps.dict_id(part.p_type);
                    assert(0 <= id < dl && ps.dtext(id) == part.p_type);
                    if id == t { assert(part.p_type == ps.dtext(t)); }
                    if part.p_type == ps.dtext(t) { lemma_dict_text_eq(ps, id, t); }
                }
                if lhs(part) != rhs(part) {
                    // one side is f(part) != 0 and the other 0
                }
            }
            lemma_sum_ext_all(db.part, lhs, rhs);
        } else {
            assert forall|index: int| 0 <= index < db.part.len() implies lhs(#[trigger] db.part[index]) == 0 by {
                let part = db.part[index];
                if type_key(ps)(part) == t {
                    assert(ps.dict_has(part.p_type));
                    let id = ps.dict_id(part.p_type);
                    assert(id == t);
                    assert(part.p_type == ps.dtext(t));
                    if f(part) != 0 { assert(like_match(part.p_type, pat)); }
                }
            }
            lemma_sum_zero(db.part, lhs);
        }
    }
    lemma_sum_ext(ks, per, target);
}


pub(crate) open spec fn cell_output(state: State, regions: Directory<Count>, size: i64, t: int) -> spec_fn(Cell) -> Seq<OutRow> {
    |cell: Cell| key_output_p(state, cell.first, seq![cell_cnt(state.parts, size, t, cell)], regions)
}

pub(crate) open spec fn cell_outputs(state: State, regions: Directory<Count>, size: i64, t: int, rows: Seq<Cell>) -> Seq<OutRow> {
    flat(rows.map_values(cell_output(state, regions, size, t)))
}

pub(crate) open spec fn type_outputs(state: State, regions: Directory<Count>, size: i64, pat: Seq<u8>, t: int) -> Seq<OutRow> {
    if like_match(state.parts.dtext(t), pat) { cell_outputs(state, regions, size, t, tl_rows(state.parts, size, t)) } else { Seq::empty() }
}

pub(crate) open spec fn type_output_fn(state: State, regions: Directory<Count>, size: i64, pat: Seq<u8>) -> spec_fn(int) -> Seq<OutRow> {
    |t: int| type_outputs(state, regions, size, pat, t)
}

pub(crate) open spec fn types_outputs(state: State, regions: Directory<Count>, size: i64, pat: Seq<u8>, n: int) -> Seq<OutRow> {
    flat(ids(n).map_values(type_output_fn(state, regions, size, pat)))
}

pub(crate) open spec fn query_weight_fn(db: Db, p: Params, weight: spec_fn(OutRow) -> int) -> spec_fn(PartRow) -> int {
    |part: PartRow| part_query_weight(db, p, part, weight)
}

pub(crate) open spec fn db_fits(db: Db) -> bool {
    forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size) && fits_i64(db.part[index].p_partkey)
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_cell_outputs_weight(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, t: int, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@, 0 <= t < state.parts.dl(), like_match(state.parts.dtext(t), pat), db_fits(state.db()),
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures sum(cell_outputs(state, regions, size, t, tl_rows(state.parts, size, t)), weight) ==
        sum(tl_rows(state.parts, size, t), |cell: Cell| cell.count as int * query_weight_fn(state.db(), params(size as int, pat, name), weight)(cell_cnt(state.parts, size, t, cell).row)),
{
    let db = state.db();
    let ps = state.parts;
    let p = params(size as int, pat, name);
    let rows = tl_rows(ps, size, t);
    let nested = |cell: Cell| sum(cell_output(state, regions, size, t)(cell), weight);
    lemma_flat_map_weight(rows, cell_output(state, regions, size, t), weight, nested);
    assert forall|index: int| 0 <= index < rows.len() implies nested(#[trigger] rows[index]) ==
        (|cell: Cell| cell.count as int * query_weight_fn(db, p, weight)(cell_cnt(ps, size, t, cell).row))(rows[index]) by {
        lemma_tl_cell_facts(db, ps, size, t, index);
        let cell = rows[index];
        let cnt = cell_cnt(ps, size, t, cell);
        assert(like_match(cnt.row.p_type, pat));
        lemma_cell_weight(state, size, name, pat, cnt, regions, weight);
    }
    lemma_sum_ext(rows, nested, |cell: Cell| cell.count as int * query_weight_fn(db, p, weight)(cell_cnt(ps, size, t, cell).row));
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_types_outputs_weight(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@, db_fits(state.db()),
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures sum(types_outputs(state, regions, size, pat, state.parts.dl()), weight) ==
        sum(state.db().part, query_weight_fn(state.db(), params(size as int, pat, name), weight)),
{
    let db = state.db();
    let ps = state.parts;
    let p = params(size as int, pat, name);
    let pqw = query_weight_fn(db, p, weight);
    let dl = ps.dl();
    let nested = |t: int| sum(type_output_fn(state, regions, size, pat)(t), weight);
    lemma_flat_map_weight(ids(dl), type_output_fn(state, regions, size, pat), weight, nested);
    assert forall|part: PartRow| pqw(part) != 0 implies part.p_size == size as int && like_match(part.p_type, pat) by {}
    lemma_types_group(db, ps, size, pat, pqw);
    let target = |t: int| if like_match(ps.dtext(t), pat) {
        sum(db.part, |part: PartRow| if part.p_size == size as int && part.p_type == ps.dtext(t) { pqw(part) } else { 0int })
    } else { 0int };
    assert forall|index: int| 0 <= index < ids(dl).len() implies nested(#[trigger] ids(dl)[index]) == target(ids(dl)[index]) by {
        let t = ids(dl)[index];
        assert(t == index);
        if like_match(ps.dtext(t), pat) {
            lemma_cell_outputs_weight(state, size, name, pat, t, regions, weight);
            lemma_row_weight(db, ps, size, t, pqw);
            assert(type_output_fn(state, regions, size, pat)(t) == cell_outputs(state, regions, size, t, tl_rows(ps, size, t)));
        } else {
            assert(type_output_fn(state, regions, size, pat)(t) =~= Seq::<OutRow>::empty());
            lemma_sum_empty(Seq::<OutRow>::empty(), weight);
        }
    }
    lemma_sum_ext(ids(dl), nested, target);
}

pub(crate) proof fn lemma_types_outputs_bag(state: State, size: i64, name: Seq<u8>, pat: Seq<u8>, regions: Directory<Count>)
    requires state.meaning(), state.db() == state.database@, db_fits(state.db()),
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures types_outputs(state, regions, size, pat, state.parts.dl()).to_multiset() == rows(state.db(), params(size as int, pat, name)).to_multiset(),
{
    let db = state.db();
    let p = params(size as int, pat, name);
    let left = types_outputs(state, regions, size, pat, state.parts.dl());
    let right = rows(db, p);
    assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(left, weight) == sum(right, weight) by {
        lemma_types_outputs_weight(state, size, name, pat, regions, weight);
        lemma_result_weight(db, p, weight);
    }
    verus_prooflib::lemma_weighted_bag_equal(left, right);
}


pub(crate) proof fn lemma_cell_outputs_step(state: State, regions: Directory<Count>, size: i64, t: int, rows: Seq<Cell>, index: int)
    requires 0 <= index < rows.len(),
    ensures cell_outputs(state, regions, size, t, rows.take(index + 1)) ==
        cell_outputs(state, regions, size, t, rows.take(index)) + cell_output(state, regions, size, t)(rows[index]),
{
    assert(rows.take(index + 1) =~= rows.take(index).push(rows[index]));
    verus_prooflib::lemma_flat_map_push(rows.take(index), rows[index], cell_output(state, regions, size, t));
}

pub(crate) proof fn lemma_types_outputs_step(state: State, regions: Directory<Count>, size: i64, pat: Seq<u8>, n: int)
    requires 0 <= n,
    ensures types_outputs(state, regions, size, pat, n + 1) ==
        types_outputs(state, regions, size, pat, n) + type_outputs(state, regions, size, pat, n),
{
    assert(ids(n + 1) =~= ids(n).push(n));
    verus_prooflib::lemma_flat_map_push(ids(n), n, type_output_fn(state, regions, size, pat));
}

}
