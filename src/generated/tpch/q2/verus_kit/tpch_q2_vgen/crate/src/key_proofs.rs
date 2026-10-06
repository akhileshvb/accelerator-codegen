use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::bags::*;
use crate::storage::*;
use crate::relational::*;
use crate::join_parts::*;

verus! {

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_join_ext(db: Db, name: Seq<u8>, key: int, left: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int, right: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int)
    requires forall|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| quote.ps_partkey == key ==> #[trigger] left(quote, supplier, nation) == right(quote, supplier, nation),
    ensures quote_join(db, name, key, left) == quote_join(db, name, key, right),
{
    let left_raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| left(quote, supplier, nation)) } else { 0int };
    let right_raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| right(quote, supplier, nation)) } else { 0int };
    assert forall|quote: PartsuppRow| #[trigger] left_raw(quote) == right_raw(quote) by {
        if quote.ps_partkey == key {
            assert((|supplier: SupplierRow, nation: NationRow| left(quote, supplier, nation)) =~= (|supplier: SupplierRow, nation: NationRow| right(quote, supplier, nation)));
        }
    }
    lemma_quote_join_weight(db, name, key, left, left_raw);
    lemma_quote_join_weight(db, name, key, right, right_raw);
    lemma_sum_ext_all(db.partsupp, left_raw, right_raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_join_zero(db: Db, name: Seq<u8>, key: int)
    ensures quote_join(db, name, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| 0int) == 0,
{
    let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| 0int;
    let raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation)) } else { 0int };
    assert forall|quote: PartsuppRow| #[trigger] raw(quote) == 0 by {
        let supplier_weight = |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation);
        let supplier_raw = |supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey { nation_join(db, name, supplier, |nation: NationRow| supplier_weight(supplier, nation)) } else { 0int };
        assert forall|supplier: SupplierRow| #[trigger] supplier_raw(supplier) == 0 by {
            let nation_weight = |nation: NationRow| supplier_weight(supplier, nation);
            lemma_nation_join_weight(db, name, supplier, nation_weight, |nation: NationRow| 0int);
            lemma_sum_zero(db.nation, |nation: NationRow| 0int);
        }
        lemma_supplier_join_weight(db, name, quote, supplier_weight, supplier_raw);
        lemma_sum_zero(db.supplier, supplier_raw);
    }
    lemma_quote_join_weight(db, name, key, joined, raw);
    lemma_sum_zero(db.partsupp, raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_join_empty(db: Db, name: Seq<u8>, key: int, weight: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int)
    requires forall|index: int| 0 <= index < db.partsupp.len() ==> (#[trigger] db.partsupp[index]).ps_partkey != key,
    ensures quote_join(db, name, key, weight) == 0,
{
    let raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)) } else { 0int };
    lemma_quote_join_weight(db, name, key, weight, raw);
    lemma_sum_zero(db.partsupp, raw);
}

}
