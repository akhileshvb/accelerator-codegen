use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::lemma_flat_map_weight;
use crate::gen_spec::*;
use crate::model::*;
use crate::relational::*;

use verus_common::sqlfns::like_match;

verus! {

pub open spec fn row_projection(part: PartRow, supplier: SupplierRow, nation: NationRow) -> OutRow {
    OutRow { s_acctbal: supplier.s_acctbal, s_name: supplier.s_name, n_name: nation.n_name,
        p_partkey: part.p_partkey, p_mfgr: part.p_mfgr, s_address: supplier.s_address,
        s_phone: supplier.s_phone, s_comment: supplier.s_comment }
}

pub open spec fn part_selected(part: PartRow, p: Params) -> bool { retained(part) && part.p_size == p.p1 && like_match(part.p_type, p.p2) }

pub open spec fn quote_selected(db: Db, p: Params, part: PartRow, quote: PartsuppRow) -> bool {
    quote.ps_partkey == part.p_partkey && scalar_1_value(db, p, part) is Some && quote.ps_supplycost == scalar_1_value(db, p, part)->0
}

pub open spec fn part_query_weight(db: Db, p: Params, part: PartRow, weight: spec_fn(OutRow) -> int) -> int {
    if part_selected(part, p) {
        quote_join(db, p.p3, part.p_partkey, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow|
            if quote_selected(db, p, part, quote) { weight(row_projection(part, supplier, nation)) } else { 0int })
    } else { 0int }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_result_nation(db: Db, p: Params, part: PartRow, supplier: SupplierRow, quote: PartsuppRow, nation: NationRow, weight: spec_fn(OutRow) -> int)
    ensures sum(tuples_4(db, p, part, supplier, quote, nation), |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation))) ==
        if part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey && supplier.s_nationkey == nation.n_nationkey {
            region_count(db, p.p3, nation.n_regionkey) * weight(row_projection(part, supplier, nation))
        } else { 0int },
{
    let tuple_weight = |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation));
    let keep = part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey && supplier.s_nationkey == nation.n_nationkey;
    let raw = |region: RegionRow| if keep && region.r_name == p.p3 && region.r_regionkey == nation.n_regionkey { weight(row_projection(part, supplier, nation)) } else { 0int };
    assert forall|region: RegionRow| #[trigger] raw(region) == sum(tuples_5(db, p, part, supplier, quote, nation, region), tuple_weight) by {
        lemma_sum_opt(where_clause(db, p, part, supplier, quote, nation, region), Tuple { part, supplier, partsupp: quote, nation, region }, tuple_weight);
    }
    lemma_flat_map_weight(db.region, |region: RegionRow| tuples_5(db, p, part, supplier, quote, nation, region), tuple_weight, raw);
    if keep {
        lemma_sum_scale(db.region, |region: RegionRow| if region.r_name == p.p3 && region.r_regionkey == nation.n_regionkey { 1int } else { 0int }, raw, weight(row_projection(part, supplier, nation)));
        assert(region_count(db, p.p3, nation.n_regionkey) * weight(row_projection(part, supplier, nation)) == weight(row_projection(part, supplier, nation)) * region_count(db, p.p3, nation.n_regionkey)) by (nonlinear_arith);
    } else { lemma_sum_zero(db.region, raw); }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_result_quote(db: Db, p: Params, part: PartRow, supplier: SupplierRow, quote: PartsuppRow, weight: spec_fn(OutRow) -> int)
    ensures sum(tuples_3(db, p, part, supplier, quote), |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation))) ==
        if part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey {
            nation_join(db, p.p3, supplier, |nation: NationRow| weight(row_projection(part, supplier, nation)))
        } else { 0int },
{
    let tuple_weight = |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation));
    let keep = part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey;
    let raw = |nation: NationRow| if keep && supplier.s_nationkey == nation.n_nationkey { region_count(db, p.p3, nation.n_regionkey) * weight(row_projection(part, supplier, nation)) } else { 0int };
    assert forall|nation: NationRow| #[trigger] raw(nation) == sum(tuples_4(db, p, part, supplier, quote, nation), tuple_weight) by {
        lemma_result_nation(db, p, part, supplier, quote, nation, weight);
    }
    lemma_flat_map_weight(db.nation, |nation: NationRow| tuples_4(db, p, part, supplier, quote, nation), tuple_weight, raw);
    if keep { lemma_nation_join_weight(db, p.p3, supplier, |nation: NationRow| weight(row_projection(part, supplier, nation)), raw); }
    else { lemma_sum_zero(db.nation, raw); }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_result_supplier(db: Db, p: Params, part: PartRow, supplier: SupplierRow, weight: spec_fn(OutRow) -> int)
    ensures sum(tuples_2(db, p, part, supplier), |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation))) ==
        sum(db.partsupp, |quote: PartsuppRow| if part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey {
            nation_join(db, p.p3, supplier, |nation: NationRow| weight(row_projection(part, supplier, nation)))
        } else { 0int }),
{
    let tuple_weight = |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation));
    let raw = |quote: PartsuppRow| if part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey {
        nation_join(db, p.p3, supplier, |nation: NationRow| weight(row_projection(part, supplier, nation)))
    } else { 0int };
    assert forall|quote: PartsuppRow| #[trigger] raw(quote) == sum(tuples_3(db, p, part, supplier, quote), tuple_weight) by {
        lemma_result_quote(db, p, part, supplier, quote, weight);
    }
    lemma_flat_map_weight(db.partsupp, |quote: PartsuppRow| tuples_3(db, p, part, supplier, quote), tuple_weight, raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_result_part(db: Db, p: Params, part: PartRow, weight: spec_fn(OutRow) -> int)
    ensures sum(tuples_1(db, p, part), |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation))) == part_query_weight(db, p, part, weight),
{
    let tuple_weight = |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation));
    let pair = |supplier: SupplierRow, quote: PartsuppRow| if part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey {
        nation_join(db, p.p3, supplier, |nation: NationRow| weight(row_projection(part, supplier, nation)))
    } else { 0int };
    let left = |supplier: SupplierRow| sum(db.partsupp, |quote: PartsuppRow| pair(supplier, quote));
    let right = |quote: PartsuppRow| sum(db.supplier, |supplier: SupplierRow| pair(supplier, quote));
    assert forall|supplier: SupplierRow| #[trigger] left(supplier) == sum(tuples_2(db, p, part, supplier), tuple_weight) by {
        lemma_result_supplier(db, p, part, supplier, weight);
        assert((|quote: PartsuppRow| pair(supplier, quote)) =~= (|quote: PartsuppRow| if part_selected(part, p) && quote_selected(db, p, part, quote) && supplier.s_suppkey == quote.ps_suppkey {
            nation_join(db, p.p3, supplier, |nation: NationRow| weight(row_projection(part, supplier, nation)))
        } else { 0int }));
    }
    lemma_flat_map_weight(db.supplier, |supplier: SupplierRow| tuples_2(db, p, part, supplier), tuple_weight, left);
    lemma_sum_swap(db.supplier, db.partsupp, pair, left, right);
    if part_selected(part, p) {
        let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, part, quote) { weight(row_projection(part, supplier, nation)) } else { 0int };
        assert forall|quote: PartsuppRow| #[trigger] right(quote) == if quote.ps_partkey == part.p_partkey {
            supplier_join(db, p.p3, quote, |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation))
        } else { 0int } by {
            let quote_weight = |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation);
            if quote_selected(db, p, part, quote) {
                assert forall|supplier: SupplierRow| #[trigger] pair(supplier, quote) == if supplier.s_suppkey == quote.ps_suppkey {
                    nation_join(db, p.p3, supplier, |nation: NationRow| quote_weight(supplier, nation))
                } else { 0int } by {
                    assert((|nation: NationRow| weight(row_projection(part, supplier, nation))) =~= (|nation: NationRow| quote_weight(supplier, nation)));
                }
                lemma_supplier_join_weight(db, p.p3, quote, quote_weight, |supplier: SupplierRow| pair(supplier, quote));
            } else {
                lemma_sum_zero(db.supplier, |supplier: SupplierRow| pair(supplier, quote));
                if quote.ps_partkey == part.p_partkey {
                    let nation_zero = |supplier: SupplierRow| nation_join(db, p.p3, supplier, |nation: NationRow| quote_weight(supplier, nation));
                    assert forall|supplier: SupplierRow| #[trigger] nation_zero(supplier) == 0 by {
                        let raw = |nation: NationRow| 0int;
                        lemma_nation_join_weight(db, p.p3, supplier, |nation: NationRow| quote_weight(supplier, nation), raw);
                        lemma_sum_zero(db.nation, raw);
                    }
                    let zero = |supplier: SupplierRow| 0int;
                    assert forall|supplier: SupplierRow| #[trigger] zero(supplier) == if supplier.s_suppkey == quote.ps_suppkey {
                        nation_join(db, p.p3, supplier, |nation: NationRow| quote_weight(supplier, nation))
                    } else { 0int } by { assert(nation_zero(supplier) == 0); }
                    lemma_supplier_join_weight(db, p.p3, quote, quote_weight, zero);
                    lemma_sum_zero(db.supplier, |supplier: SupplierRow| 0int);
                }
            }
        }
        lemma_quote_join_weight(db, p.p3, part.p_partkey, joined, right);
    } else {
        assert forall|quote: PartsuppRow| #[trigger] right(quote) == 0 by {
            lemma_sum_zero(db.supplier, |supplier: SupplierRow| pair(supplier, quote));
        }
        lemma_sum_zero(db.partsupp, right);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_result_weight(db: Db, p: Params, weight: spec_fn(OutRow) -> int)
    ensures sum(rows(db, p), weight) == sum(db.part, |part: PartRow| part_query_weight(db, p, part, weight)),
{
    let tuple_weight = |t: Tuple| weight(row_projection(t.part, t.supplier, t.nation));
    let raw = |part: PartRow| part_query_weight(db, p, part, weight);
    assert forall|part: PartRow| #[trigger] raw(part) == sum(tuples_1(db, p, part), tuple_weight) by {
        lemma_result_part(db, p, part, weight);
    }
    lemma_flat_map_weight(db.part, |part: PartRow| tuples_1(db, p, part), tuple_weight, raw);
    lemma_sum_map(tuples(db, p), |t: Tuple| row_projection(t.part, t.supplier, t.nation), weight, tuple_weight);
    assert(rows(db, p) =~= tuples(db, p).map_values(|t: Tuple| row_projection(t.part, t.supplier, t.nation)));
}

}
