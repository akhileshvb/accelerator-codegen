use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::query_proofs::*;
use crate::emission::*;
use crate::maintenance::{variant_count, variants_shape};

verus! {
pub open spec fn supplier_output(row: SupplierRow, revenue: int) -> OutRow {
    OutRow { s_suppkey: row.s_suppkey, s_name: row.s_name, s_address: row.s_address, s_phone: row.s_phone, total_revenue: revenue }
}
pub open spec fn tuple_output(tuple: Tuple) -> OutRow {
    supplier_output(tuple.supplier, tuple.revenue0.total_revenue)
}
pub open spec fn tuple_weight(output: OutRow) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if tuple_output(tuple) == output { 1int } else { 0int }
}
pub open spec fn supplier_output_weight(winners: Seq<i64>, revenue: int, output: OutRow) -> spec_fn(SupplierRow) -> int {
    |row: SupplierRow| if has_winner(winners, row.s_suppkey) && supplier_output(row, revenue) == output { 1int } else { 0int }
}
#[verifier::spinoff_prover]
pub proof fn lemma_repeat_count(row: OutRow, count: int, output: OutRow)
    requires count >= 0,
    ensures sum(repeated(row, count), eqv(output)) == count * eqv(output)(row),
    decreases count,
{
    if count > 0 {
        lemma_repeat_count(row, count - 1, output);
        lemma_repeat_push(row, count - 1);
        lemma_sum_push(repeated(row, count - 1), row, eqv(output));
        let unit = eqv(output)(row);
        assert(count * unit == (count - 1) * unit + unit) by (nonlinear_arith);
    }
}
pub proof fn lemma_inner_count(variants: &Index<Variant>, supplier: int, revenue: int, output: OutRow)
    requires variants.wf(), variants_shape(*variants),
    ensures sum(inner_rows(supplier, *variants, revenue), eqv(output)) ==
        if output.s_suppkey == supplier && output.total_revenue == revenue {
            variant_count(*variants, variant_key(output.s_name, output.s_address, output.s_phone))
        } else { 0int },
{
    variants.model();
    let entries = variants.entries();
    let mapper = |entry: (KeyView, Variant)| variant_rows(supplier, entry.1, revenue);
    let per_entry = |entry: (KeyView, Variant)| sum(mapper(entry), eqv(output));
    lemma_sum_flat_map(entries, mapper, eqv(output));
    assert forall|entry: (KeyView, Variant)| #[trigger] per_entry(entry) == entry.1.count as int * eqv(output)(variant_output(supplier, entry.1, revenue)) by {
        lemma_repeat_count(variant_output(supplier, entry.1, revenue), entry.1.count as int, output);
    }
    if output.s_suppkey == supplier && output.total_revenue == revenue {
        let key = variant_key(output.s_name, output.s_address, output.s_phone);
        assert forall|position: int| 0 <= position < entries.len() implies per_entry(#[trigger] entries[position]) ==
            if entries[position].0 == key { entries[position].1.count as int } else { 0int } by {
            let entry = entries[position];
            assert(variants.at(entry.0) == Some(entry.1));
            assert(entry.0 == variant_key(entry.1.name@, entry.1.address@, entry.1.phone@));
            if entry.0 == key {
                lemma_variant_injective(entry.1.name@, entry.1.address@, entry.1.phone@, output.s_name, output.s_address, output.s_phone);
                assert(variant_output(supplier, entry.1, revenue) == output);
            } else { assert(variant_output(supplier, entry.1, revenue) != output); }
        }
        if variants.at(key) is Some {
            let position = choose|position: int| 0 <= position < entries.len() && (#[trigger] entries[position]).0 == key;
            assert forall|other: int| 0 <= other < entries.len() && other != position implies per_entry(#[trigger] entries[other]) == 0 by {
                verus_prooflib::ordered::lemma_key_order(key, key);
                if other < position { assert(verus_prooflib::ordered::key_lt(entries[other].0, entries[position].0)); }
                else { assert(verus_prooflib::ordered::key_lt(entries[position].0, entries[other].0)); }
            }
            lemma_sum_single_match(entries, per_entry, position);
            assert(entries[position].1 == variants.at(key).unwrap());
        } else {
            assert forall|position: int| 0 <= position < entries.len() implies per_entry(#[trigger] entries[position]) == 0 by {
                assert(variants.at(entries[position].0) is Some);
                assert(entries[position].0 != key);
            }
            lemma_sum_zero(entries, per_entry);
        }
    } else {
        assert forall|position: int| 0 <= position < entries.len() implies per_entry(#[trigger] entries[position]) == 0 by { }
        lemma_sum_zero(entries, per_entry);
    }
}
pub proof fn lemma_supplier_emission_count(dimensions: &Dimensions, supplier: int, revenue: int, output: OutRow)
    requires dimensions.wf(), dimensions_shape(*dimensions),
    ensures sum(supplier_rows(*dimensions, supplier, revenue), eqv(output)) ==
        if output.s_suppkey == supplier && output.total_revenue == revenue {
            stored_supplier_count(*dimensions, supplier, variant_key(output.s_name, output.s_address, output.s_phone))
        } else { 0int },
{
    if dimensions.at(number_key(supplier)) is Some {
        let inner = dimensions.at(number_key(supplier)).unwrap();
        assert(variants_shape(inner)) by {
            assert forall|key: KeyView| #[trigger] inner.at(key) is Some implies inner.at(key).unwrap().count > 0 && key == variant_key(inner.at(key).unwrap().name@, inner.at(key).unwrap().address@, inner.at(key).unwrap().phone@) by {
                let value = inner.at(key).unwrap();
                assert(variant_at(*dimensions, supplier, key) == Some(value));
            }
        }
        lemma_inner_count(&inner, supplier, revenue, output);
    }
}
pub proof fn lemma_emitted_count(dimensions: &Dimensions, suppliers: Seq<SupplierRow>, winners: Seq<i64>, revenue: int, output: OutRow)
    requires dimensions.wf(), dimensions_match(*dimensions, suppliers),
        forall|left: int, right: int| #![trigger winners[left], winners[right]] 0 <= left < right < winners.len() ==> winners[left] < winners[right],
    ensures emitted_rows(*dimensions, winners, revenue).to_multiset().count(output) as int == sum(suppliers, supplier_output_weight(winners, revenue, output)),
{
    let rows = emitted_rows(*dimensions, winners, revenue);
    lemma_count_as_sum(rows, output);
    let mapper = |supplier: i64| supplier_rows(*dimensions, supplier as int, revenue);
    let per_supplier = |supplier: i64| sum(mapper(supplier), eqv(output));
    lemma_sum_flat_map(winners, mapper, eqv(output));
    assert forall|supplier: i64| #[trigger] per_supplier(supplier) ==
        if output.s_suppkey == supplier as int && output.total_revenue == revenue {
            stored_supplier_count(*dimensions, supplier as int, variant_key(output.s_name, output.s_address, output.s_phone))
        } else { 0int } by { lemma_supplier_emission_count(dimensions, supplier as int, revenue, output); }
    let expected = supplier_output_weight(winners, revenue, output);
    if has_winner(winners, output.s_suppkey) && output.total_revenue == revenue {
        let position = choose|position: int| 0 <= position < winners.len() && (#[trigger] winners[position]) as int == output.s_suppkey;
        assert forall|other: int| 0 <= other < winners.len() && other != position implies per_supplier(#[trigger] winners[other]) == 0 by {
            if other < position { assert(winners[other] < winners[position]); }
            else { assert(winners[position] < winners[other]); }
        }
        lemma_sum_single_match(winners, per_supplier, position);
        let key = variant_key(output.s_name, output.s_address, output.s_phone);
        let count = |supplier: SupplierRow| if supplier.s_suppkey == output.s_suppkey && variant_key(supplier.s_name, supplier.s_address, supplier.s_phone) == key { 1int } else { 0int };
        assert forall|supplier: SupplierRow| #[trigger] expected(supplier) == count(supplier) by {
            if supplier.s_suppkey == output.s_suppkey && variant_key(supplier.s_name, supplier.s_address, supplier.s_phone) == key {
                lemma_variant_injective(supplier.s_name, supplier.s_address, supplier.s_phone, output.s_name, output.s_address, output.s_phone);
                assert(supplier_output(supplier, revenue) == output);
            }
        }
        lemma_sum_ext_all(suppliers, expected, count);
        assert(stored_supplier_count(*dimensions, output.s_suppkey, key) == supplier_count(suppliers, output.s_suppkey, key));
    } else {
        assert forall|position: int| 0 <= position < winners.len() implies per_supplier(#[trigger] winners[position]) == 0 by {
            if output.total_revenue == revenue { assert(winners[position] as int != output.s_suppkey); }
        }
        lemma_sum_zero(winners, per_supplier);
        assert forall|supplier: SupplierRow| #[trigger] expected(supplier) == 0 by {
            if supplier_output(supplier, revenue) == output { assert(supplier.s_suppkey == output.s_suppkey); }
        }
        lemma_sum_zero(suppliers, expected);
    }
}
}
