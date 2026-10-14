use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::relational::*;

verus! {

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_nation_parts(db: Db, name: Seq<u8>, supplier: SupplierRow, keep: spec_fn(PartRow) -> bool, weight: spec_fn(PartRow, NationRow) -> int)
    ensures nation_join(db, name, supplier, |nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { weight(part, nation) } else { 0int })) ==
        sum(db.part, |part: PartRow| if keep(part) { nation_join(db, name, supplier, |nation: NationRow| weight(part, nation)) } else { 0int }),
{
    let joined = |nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { weight(part, nation) } else { 0int });
    let pair = |part: PartRow, nation: NationRow| if keep(part) && nation.n_nationkey == supplier.s_nationkey { region_count(db, name, nation.n_regionkey) * weight(part, nation) } else { 0int };
    let left = |part: PartRow| if keep(part) { nation_join(db, name, supplier, |nation: NationRow| weight(part, nation)) } else { 0int };
    let right = |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey { region_count(db, name, nation.n_regionkey) * joined(nation) } else { 0int };
    assert forall|part: PartRow| #[trigger] left(part) == sum(db.nation, |nation: NationRow| pair(part, nation)) by {
        if keep(part) { lemma_nation_join_weight(db, name, supplier, |nation: NationRow| weight(part, nation), |nation: NationRow| pair(part, nation)); }
        else { lemma_sum_zero(db.nation, |nation: NationRow| pair(part, nation)); }
    }
    assert forall|nation: NationRow| #[trigger] right(nation) == sum(db.part, |part: PartRow| pair(part, nation)) by {
        if nation.n_nationkey == supplier.s_nationkey {
            lemma_sum_scale(db.part, |part: PartRow| if keep(part) { weight(part, nation) } else { 0int },
                |part: PartRow| pair(part, nation), region_count(db, name, nation.n_regionkey));
        } else { lemma_sum_zero(db.part, |part: PartRow| pair(part, nation)); }
    }
    lemma_sum_swap(db.part, db.nation, pair, left, right);
    lemma_nation_join_weight(db, name, supplier, joined, right);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_supplier_parts(db: Db, name: Seq<u8>, quote: PartsuppRow, keep: spec_fn(PartRow) -> bool, weight: spec_fn(PartRow, SupplierRow, NationRow) -> int)
    ensures supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { weight(part, supplier, nation) } else { 0int })) ==
        sum(db.part, |part: PartRow| if keep(part) { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(part, supplier, nation)) } else { 0int }),
{
    let joined = |supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { weight(part, supplier, nation) } else { 0int });
    let pair = |part: PartRow, supplier: SupplierRow| if keep(part) && supplier.s_suppkey == quote.ps_suppkey {
        nation_join(db, name, supplier, |nation: NationRow| weight(part, supplier, nation))
    } else { 0int };
    let left = |part: PartRow| if keep(part) { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(part, supplier, nation)) } else { 0int };
    let right = |supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey { nation_join(db, name, supplier, |nation: NationRow| joined(supplier, nation)) } else { 0int };
    assert forall|part: PartRow| #[trigger] left(part) == sum(db.supplier, |supplier: SupplierRow| pair(part, supplier)) by {
        if keep(part) {
            let part_weight = |supplier: SupplierRow, nation: NationRow| weight(part, supplier, nation);
            assert forall|supplier: SupplierRow| #[trigger] pair(part, supplier) == if supplier.s_suppkey == quote.ps_suppkey {
                nation_join(db, name, supplier, |nation: NationRow| part_weight(supplier, nation))
            } else { 0int } by {
                assert((|nation: NationRow| weight(part, supplier, nation)) =~= (|nation: NationRow| part_weight(supplier, nation)));
            }
            lemma_supplier_join_weight(db, name, quote, part_weight, |supplier: SupplierRow| pair(part, supplier));
        } else { lemma_sum_zero(db.supplier, |supplier: SupplierRow| pair(part, supplier)); }
    }
    assert forall|supplier: SupplierRow| #[trigger] right(supplier) == sum(db.part, |part: PartRow| pair(part, supplier)) by {
        if supplier.s_suppkey == quote.ps_suppkey {
            let nation_weight = |part: PartRow, nation: NationRow| weight(part, supplier, nation);
            lemma_nation_parts(db, name, supplier, keep, nation_weight);
            assert forall|nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { nation_weight(part, nation) } else { 0int }) == #[trigger] joined(supplier, nation) by {
                assert((|part: PartRow| if keep(part) { nation_weight(part, nation) } else { 0int }) =~= (|part: PartRow| if keep(part) { weight(part, supplier, nation) } else { 0int }));
            }
            assert((|nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { nation_weight(part, nation) } else { 0int })) =~= (|nation: NationRow| joined(supplier, nation)));
            assert((|part: PartRow| if keep(part) { nation_join(db, name, supplier, |nation: NationRow| nation_weight(part, nation)) } else { 0int }) =~= (|part: PartRow| pair(part, supplier))) by {
                assert forall|part: PartRow| (if keep(part) { nation_join(db, name, supplier, |nation: NationRow| nation_weight(part, nation)) } else { 0int }) == #[trigger] pair(part, supplier) by {
                    assert((|nation: NationRow| nation_weight(part, nation)) =~= (|nation: NationRow| weight(part, supplier, nation)));
                }
            }
        } else { lemma_sum_zero(db.part, |part: PartRow| pair(part, supplier)); }
    }
    lemma_sum_swap(db.part, db.supplier, pair, left, right);
    lemma_supplier_join_weight(db, name, quote, joined, right);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_parts(db: Db, name: Seq<u8>, key: int, keep: spec_fn(PartRow) -> bool, weight: spec_fn(PartRow, PartsuppRow, SupplierRow, NationRow) -> int)
    ensures quote_join(db, name, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { weight(part, quote, supplier, nation) } else { 0int })) ==
        sum(db.part, |part: PartRow| if keep(part) { quote_join(db, name, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation)) } else { 0int }),
{
    let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { weight(part, quote, supplier, nation) } else { 0int });
    let pair = |part: PartRow, quote: PartsuppRow| if keep(part) && quote.ps_partkey == key {
        supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation))
    } else { 0int };
    let left = |part: PartRow| if keep(part) { quote_join(db, name, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation)) } else { 0int };
    let right = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation)) } else { 0int };
    assert forall|part: PartRow| #[trigger] left(part) == sum(db.partsupp, |quote: PartsuppRow| pair(part, quote)) by {
        if keep(part) {
            let part_weight = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation);
            assert forall|quote: PartsuppRow| #[trigger] pair(part, quote) == if quote.ps_partkey == key {
                supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| part_weight(quote, supplier, nation))
            } else { 0int } by {
                assert((|supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation)) =~= (|supplier: SupplierRow, nation: NationRow| part_weight(quote, supplier, nation)));
            }
            lemma_quote_join_weight(db, name, key, part_weight, |quote: PartsuppRow| pair(part, quote));
        } else { lemma_sum_zero(db.partsupp, |quote: PartsuppRow| pair(part, quote)); }
    }
    assert forall|quote: PartsuppRow| #[trigger] right(quote) == sum(db.part, |part: PartRow| pair(part, quote)) by {
        if quote.ps_partkey == key {
            let supplier_weight = |part: PartRow, supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation);
            lemma_supplier_parts(db, name, quote, keep, supplier_weight);
            assert forall|supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { supplier_weight(part, supplier, nation) } else { 0int }) == #[trigger] joined(quote, supplier, nation) by {
                assert((|part: PartRow| if keep(part) { supplier_weight(part, supplier, nation) } else { 0int }) =~= (|part: PartRow| if keep(part) { weight(part, quote, supplier, nation) } else { 0int }));
            }
            assert((|supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { supplier_weight(part, supplier, nation) } else { 0int })) =~= (|supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation)));
            assert((|part: PartRow| if keep(part) { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| supplier_weight(part, supplier, nation)) } else { 0int }) =~= (|part: PartRow| pair(part, quote))) by {
                assert forall|part: PartRow| (if keep(part) { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| supplier_weight(part, supplier, nation)) } else { 0int }) == #[trigger] pair(part, quote) by {
                    assert((|supplier: SupplierRow, nation: NationRow| supplier_weight(part, supplier, nation)) =~= (|supplier: SupplierRow, nation: NationRow| weight(part, quote, supplier, nation)));
                }
            }
        } else { lemma_sum_zero(db.part, |part: PartRow| pair(part, quote)); }
    }
    lemma_sum_swap(db.part, db.partsupp, pair, left, right);
    lemma_quote_join_weight(db, name, key, joined, right);
}

}
