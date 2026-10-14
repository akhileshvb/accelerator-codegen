use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::lemma_flat_map_weight;
use crate::gen_spec::*;
use crate::model::*;
use crate::storage::*;
use crate::bags::*;
use crate::maintenance::*;
use crate::query_transfer::*;

verus! {

pub open spec fn region_count(db: Db, name: Seq<u8>, key: int) -> int {
    sum(db.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key { 1int } else { 0int })
}

pub open spec fn nation_join(db: Db, name: Seq<u8>, supplier: SupplierRow, weight: spec_fn(NationRow) -> int) -> int {
    sum(db.nation, |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey {
        region_count(db, name, nation.n_regionkey) * weight(nation)
    } else { 0int })
}

pub open spec fn supplier_join(db: Db, name: Seq<u8>, quote: PartsuppRow, weight: spec_fn(SupplierRow, NationRow) -> int) -> int {
    sum(db.supplier, |supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey {
        nation_join(db, name, supplier, |nation: NationRow| weight(supplier, nation))
    } else { 0int })
}

pub open spec fn quote_join(db: Db, name: Seq<u8>, key: int, weight: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int) -> int {
    sum(db.partsupp, |quote: PartsuppRow| if quote.ps_partkey == key {
        supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation))
    } else { 0int })
}

pub proof fn lemma_nation_join_weight(db: Db, name: Seq<u8>, supplier: SupplierRow, weight: spec_fn(NationRow) -> int, raw: spec_fn(NationRow) -> int)
    requires forall|nation: NationRow| #[trigger] raw(nation) == if nation.n_nationkey == supplier.s_nationkey { region_count(db, name, nation.n_regionkey) * weight(nation) } else { 0int },
    ensures nation_join(db, name, supplier, weight) == sum(db.nation, raw),
{
    assert((|nation: NationRow| if nation.n_nationkey == supplier.s_nationkey { region_count(db, name, nation.n_regionkey) * weight(nation) } else { 0int }) =~= raw);
}

pub proof fn lemma_supplier_join_weight(db: Db, name: Seq<u8>, quote: PartsuppRow, weight: spec_fn(SupplierRow, NationRow) -> int, raw: spec_fn(SupplierRow) -> int)
    requires forall|supplier: SupplierRow| #[trigger] raw(supplier) == if supplier.s_suppkey == quote.ps_suppkey { nation_join(db, name, supplier, |nation: NationRow| weight(supplier, nation)) } else { 0int },
    ensures supplier_join(db, name, quote, weight) == sum(db.supplier, raw),
{
    assert((|supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey { nation_join(db, name, supplier, |nation: NationRow| weight(supplier, nation)) } else { 0int }) =~= raw);
}

pub proof fn lemma_quote_join_weight(db: Db, name: Seq<u8>, key: int, weight: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int, raw: spec_fn(PartsuppRow) -> int)
    requires forall|quote: PartsuppRow| #[trigger] raw(quote) == if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)) } else { 0int },
    ensures quote_join(db, name, key, weight) == sum(db.partsupp, raw),
{
    assert((|quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)) } else { 0int }) =~= raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_scalar_nation(db: Db, p: Params, part: PartRow, quote: PartsuppRow, supplier: SupplierRow, nation: NationRow,
    weight: spec_fn(int) -> int)
    ensures sum(scalar_1_tuples_3(db, p, part, quote, supplier, nation), |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost)) ==
        if part.p_partkey == quote.ps_partkey && supplier.s_suppkey == quote.ps_suppkey && supplier.s_nationkey == nation.n_nationkey {
            region_count(db, p.p3, nation.n_regionkey) * weight(quote.ps_supplycost)
        } else { 0int },
{
    let tuple_weight = |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost);
    let keep = part.p_partkey == quote.ps_partkey && supplier.s_suppkey == quote.ps_suppkey && supplier.s_nationkey == nation.n_nationkey;
    let raw = |region: RegionRow| if keep && region.r_name == p.p3 && region.r_regionkey == nation.n_regionkey { weight(quote.ps_supplycost) } else { 0int };
    assert forall|region: RegionRow| #[trigger] raw(region) == sum(scalar_1_tuples_4(db, p, part, quote, supplier, nation, region), tuple_weight) by {
        lemma_sum_opt(scalar_1_where_clause(db, p, part, quote, supplier, nation, region),
            Scalar1Tuple { partsupp_2: quote, supplier_2: supplier, nation_2: nation, region_2: region }, tuple_weight);
    }
    lemma_flat_map_weight(db.region, |region: RegionRow| scalar_1_tuples_4(db, p, part, quote, supplier, nation, region), tuple_weight, raw);
    if keep {
        lemma_sum_scale(db.region, |region: RegionRow| if region.r_name == p.p3 && region.r_regionkey == nation.n_regionkey { 1int } else { 0int }, raw, weight(quote.ps_supplycost));
        assert(region_count(db, p.p3, nation.n_regionkey) * weight(quote.ps_supplycost) ==
            weight(quote.ps_supplycost) * region_count(db, p.p3, nation.n_regionkey)) by (nonlinear_arith);
    } else { lemma_sum_zero(db.region, raw); }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_scalar_supplier(db: Db, p: Params, part: PartRow, quote: PartsuppRow, supplier: SupplierRow, weight: spec_fn(int) -> int)
    ensures sum(scalar_1_tuples_2(db, p, part, quote, supplier), |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost)) ==
        if part.p_partkey == quote.ps_partkey && supplier.s_suppkey == quote.ps_suppkey {
            nation_join(db, p.p3, supplier, |nation: NationRow| weight(quote.ps_supplycost))
        } else { 0int },
{
    let tuple_weight = |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost);
    let raw = |nation: NationRow| if part.p_partkey == quote.ps_partkey && supplier.s_suppkey == quote.ps_suppkey && supplier.s_nationkey == nation.n_nationkey {
        region_count(db, p.p3, nation.n_regionkey) * weight(quote.ps_supplycost)
    } else { 0int };
    assert forall|nation: NationRow| #[trigger] raw(nation) == sum(scalar_1_tuples_3(db, p, part, quote, supplier, nation), tuple_weight) by {
        lemma_scalar_nation(db, p, part, quote, supplier, nation, weight);
    }
    lemma_flat_map_weight(db.nation, |nation: NationRow| scalar_1_tuples_3(db, p, part, quote, supplier, nation), tuple_weight, raw);
    if part.p_partkey != quote.ps_partkey || supplier.s_suppkey != quote.ps_suppkey { lemma_sum_zero(db.nation, raw); }
    else { lemma_nation_join_weight(db, p.p3, supplier, |nation: NationRow| weight(quote.ps_supplycost), raw); }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_scalar_quote(db: Db, p: Params, part: PartRow, quote: PartsuppRow, weight: spec_fn(int) -> int)
    ensures sum(scalar_1_tuples_1(db, p, part, quote), |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost)) ==
        if part.p_partkey == quote.ps_partkey {
            supplier_join(db, p.p3, quote, |supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost))
        } else { 0int },
{
    let tuple_weight = |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost);
    let raw = |supplier: SupplierRow| if part.p_partkey == quote.ps_partkey && supplier.s_suppkey == quote.ps_suppkey {
        nation_join(db, p.p3, supplier, |nation: NationRow| weight(quote.ps_supplycost))
    } else { 0int };
    assert forall|supplier: SupplierRow| #[trigger] raw(supplier) == sum(scalar_1_tuples_2(db, p, part, quote, supplier), tuple_weight) by {
        lemma_scalar_supplier(db, p, part, quote, supplier, weight);
    }
    lemma_flat_map_weight(db.supplier, |supplier: SupplierRow| scalar_1_tuples_2(db, p, part, quote, supplier), tuple_weight, raw);
    if part.p_partkey != quote.ps_partkey { lemma_sum_zero(db.supplier, raw); }
    else {
        assert forall|supplier: SupplierRow| #[trigger] raw(supplier) == if supplier.s_suppkey == quote.ps_suppkey {
            nation_join(db, p.p3, supplier, |nation: NationRow| (|supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost))(supplier, nation))
        } else { 0int } by {
            assert((|nation: NationRow| weight(quote.ps_supplycost)) =~= (|nation: NationRow| (|supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost))(supplier, nation)));
        }
        lemma_supplier_join_weight(db, p.p3, quote, |supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost), raw);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_scalar_weight(db: Db, p: Params, part: PartRow, weight: spec_fn(int) -> int)
    ensures sum(scalar_1_group_vals(db, p, part, |t: Scalar1Tuple| t.partsupp_2.ps_supplycost), weight) ==
        quote_join(db, p.p3, part.p_partkey, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost)),
{
    let tuple_weight = |t: Scalar1Tuple| weight(t.partsupp_2.ps_supplycost);
    let raw = |quote: PartsuppRow| if part.p_partkey == quote.ps_partkey {
        supplier_join(db, p.p3, quote, |supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost))
    } else { 0int };
    assert forall|quote: PartsuppRow| #[trigger] raw(quote) == sum(scalar_1_tuples_1(db, p, part, quote), tuple_weight) by {
        lemma_scalar_quote(db, p, part, quote, weight);
    }
    lemma_flat_map_weight(db.partsupp, |quote: PartsuppRow| scalar_1_tuples_1(db, p, part, quote), tuple_weight, raw);
    lemma_sum_map(scalar_1_tuples(db, p, part), |t: Scalar1Tuple| t.partsupp_2.ps_supplycost, weight, tuple_weight);
    assert forall|quote: PartsuppRow| #[trigger] raw(quote) == if quote.ps_partkey == part.p_partkey {
        supplier_join(db, p.p3, quote, |supplier: SupplierRow, nation: NationRow| (|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost))(quote, supplier, nation))
    } else { 0int } by {
        assert((|supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost)) =~= (|supplier: SupplierRow, nation: NationRow| (|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost))(quote, supplier, nation)));
    }
    lemma_quote_join_weight(db, p.p3, part.p_partkey, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost), raw);
}

pub(crate) open spec fn index_nation(state: State, regions: Directory<Count>, key: i64, weight: spec_fn(NationRow) -> int) -> int {
    index_weight(state.nations, key, |nation: NationRow| count_at(regions, nation.n_regionkey as i64) * weight(nation))
}

pub(crate) open spec fn index_supplier(state: State, regions: Directory<Count>, key: i64, weight: spec_fn(SupplierRow, NationRow) -> int) -> int {
    index_weight(state.suppliers, key, |supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| weight(supplier, nation)))
}

pub(crate) open spec fn index_quote(state: State, regions: Directory<Count>, key: i64, weight: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int) -> int {
    index_weight(state.quotes, key, |quote: PartsuppRow| index_supplier(state, regions, quote.ps_suppkey as i64, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)))
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_index_nation(state: State, regions: Directory<Count>, name: Seq<u8>, supplier: SupplierRow, weight: spec_fn(NationRow) -> int)
    requires state.meaning(), state.db() == state.database@, fits_i64(supplier.s_nationkey),
        forall|key: i64| #[trigger] count_at(regions, key) == region_amount(state.regions, name, key),
    ensures index_nation(state, regions, supplier.s_nationkey as i64, weight) == nation_join(state.db(), name, supplier, weight),
{
    let db = state.db();
    let raw = |nation: NationRow| count_at(regions, nation.n_regionkey as i64) * weight(nation);
    lemma_nation_weight(state, supplier.s_nationkey as i64, raw);
    assert forall|index: int| 0 <= index < db.nation.len() implies
        (if (#[trigger] db.nation[index]).n_nationkey == supplier.s_nationkey { raw(db.nation[index]) } else { 0int }) ==
        (if db.nation[index].n_nationkey == supplier.s_nationkey { region_count(db, name, db.nation[index].n_regionkey) * weight(db.nation[index]) } else { 0int }) by {
        let nation = db.nation[index];
        assert(fits_i64(nation.n_regionkey));
        assert(region_amount(state.regions, name, nation.n_regionkey as i64) == region_count(db, name, nation.n_regionkey));
    }
    lemma_sum_ext(db.nation, |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey { raw(nation) } else { 0int },
        |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey { region_count(db, name, nation.n_regionkey) * weight(nation) } else { 0int });
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_index_supplier(state: State, regions: Directory<Count>, name: Seq<u8>, quote: PartsuppRow, weight: spec_fn(SupplierRow, NationRow) -> int)
    requires state.meaning(), state.db() == state.database@, fits_i64(quote.ps_suppkey),
        forall|key: i64| #[trigger] count_at(regions, key) == region_amount(state.regions, name, key),
    ensures index_supplier(state, regions, quote.ps_suppkey as i64, weight) == supplier_join(state.db(), name, quote, weight),
{
    let db = state.db();
    let raw = |supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| weight(supplier, nation));
    lemma_supplier_weight(state, quote.ps_suppkey as i64, raw);
    assert forall|index: int| 0 <= index < db.supplier.len() implies
        (if (#[trigger] db.supplier[index]).s_suppkey == quote.ps_suppkey { raw(db.supplier[index]) } else { 0int }) ==
        (if db.supplier[index].s_suppkey == quote.ps_suppkey { nation_join(db, name, db.supplier[index], |nation: NationRow| weight(db.supplier[index], nation)) } else { 0int }) by {
        lemma_index_nation(state, regions, name, db.supplier[index], |nation: NationRow| weight(db.supplier[index], nation));
    }
    lemma_sum_ext(db.supplier, |supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey { raw(supplier) } else { 0int },
        |supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey { nation_join(db, name, supplier, |nation: NationRow| weight(supplier, nation)) } else { 0int });
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_index_quote(state: State, regions: Directory<Count>, name: Seq<u8>, key: i64, weight: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int)
    requires state.meaning(), state.db() == state.database@, eligible(state.db(), key) > 0,
        forall|key: i64| #[trigger] count_at(regions, key) == region_amount(state.regions, name, key),
    ensures index_quote(state, regions, key, weight) == quote_join(state.db(), name, key as int, weight),
{
    let db = state.db();
    let raw = |quote: PartsuppRow| index_supplier(state, regions, quote.ps_suppkey as i64, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation));
    lemma_quote_weight(state, key, raw);
    assert forall|index: int| 0 <= index < db.partsupp.len() implies
        (if (#[trigger] db.partsupp[index]).ps_partkey == key as int { raw(db.partsupp[index]) } else { 0int }) ==
        (if db.partsupp[index].ps_partkey == key as int { supplier_join(db, name, db.partsupp[index], |supplier: SupplierRow, nation: NationRow| weight(db.partsupp[index], supplier, nation)) } else { 0int }) by {
        let quote = db.partsupp[index];
        if quote.ps_partkey == key as int {
            if !(exists|position: int| 0 <= position < db.part.len() && (#[trigger] db.part[position]).p_partkey == key as int && retained(db.part[position])) {
                lemma_sum_zero(db.part, |part: PartRow| if part.p_partkey == key as int && retained(part) { 1int } else { 0int });
            }
            assert(crate::gen_obligations::scan_brass_partsupp_exists_1(db, quote));
            lemma_index_supplier(state, regions, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation));
        }
    }
    lemma_sum_ext(db.partsupp, |quote: PartsuppRow| if quote.ps_partkey == key as int { raw(quote) } else { 0int },
        |quote: PartsuppRow| if quote.ps_partkey == key as int { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)) } else { 0int });
}

}
