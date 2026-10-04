use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::order::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::input_proofs::supplier_blocker_weight;

verus! {

pub open spec fn selected_group(params: Params, brand: Seq<u8>, size: int) -> bool {
    brand != params.p1 && (size == params.p2 || size == params.p3 || size == params.p4
        || size == params.p5 || size == params.p6 || size == params.p7 || size == params.p8 || size == params.p9)
}

pub proof fn lemma_blocker_members(db: Db, params: Params, tuple: In1Tuple)
    ensures in_1_tuples(db, params).contains(tuple) <==>
        db.supplier.contains(tuple.supplier) && in_1_where_clause(db, params, tuple.supplier),
{
    let mapped = db.supplier.map_values(|supplier: SupplierRow| in_1_tuples_1(db, params, supplier));
    lemma_flat_contains(mapped, tuple);
    if in_1_tuples(db, params).contains(tuple) {
        let position = choose|position: int| 0 <= position < mapped.len() && (#[trigger] mapped[position]).contains(tuple);
        assert(tuple.supplier == db.supplier[position]);
        assert(db.supplier.contains(tuple.supplier));
    }
    if db.supplier.contains(tuple.supplier) && in_1_where_clause(db, params, tuple.supplier) {
        let position = choose|position: int| 0 <= position < db.supplier.len()
            && #[trigger] db.supplier[position] == tuple.supplier;
        assert(mapped[position] =~= seq![tuple]);
        assert(mapped[position].len() == 1);
        assert(mapped[position][0] == tuple);
        assert(mapped[position].contains(tuple));
    }
}

pub proof fn lemma_blocked(db: Db, params: Params, supplier: int)
    ensures blocker_count(db, supplier) >= 0,
        in_1(db, params, supplier) <==> blocker_count(db, supplier) > 0,
{
    let weight = |row: SupplierRow| supplier_blocker_weight(row, supplier);
    let count = |row: SupplierRow| if row.s_suppkey == supplier && in_1_where_clause(db, params, row) { 1int } else { 0int };
    let raw = |row: SupplierRow| if row.s_suppkey == supplier && verus_common::sqlfns::like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int };
    lemma_sum_ext_all(db.supplier, weight, raw);
    lemma_sum_ext_all(db.supplier, weight, count);
    lemma_sum_nonneg(db.supplier, count);
    if in_1(db, params, supplier) {
        let position = choose|position: int| 0 <= position < in_1_rows(db, params).len()
            && supplier == (#[trigger] in_1_rows(db, params)[position]).s_suppkey;
        let tuple = in_1_tuples(db, params)[position];
        assert(in_1_tuples(db, params).contains(tuple));
        lemma_blocker_members(db, params, tuple);
        let source = choose|source: int| 0 <= source < db.supplier.len()
            && #[trigger] db.supplier[source] == tuple.supplier;
        lemma_sum_pos(db.supplier, count, source);
    }
    if blocker_count(db, supplier) > 0 {
        let source = lemma_sum_nonzero_witness(db.supplier, count);
        let tuple = In1Tuple { supplier: db.supplier[source] };
        assert(db.supplier.contains(tuple.supplier));
        lemma_blocker_members(db, params, tuple);
        let position = choose|position: int| 0 <= position < in_1_tuples(db, params).len()
            && #[trigger] in_1_tuples(db, params)[position] == tuple;
        assert(in_1_rows(db, params)[position].s_suppkey == supplier);
        assert(in_1(db, params, supplier));
    }
}

pub proof fn lemma_inner_members(db: Db, params: Params, edge: PartsuppRow, tuple: Tuple)
    ensures tuples_1(db, params, edge).contains(tuple) <==>
        tuple.partsupp == edge && db.part.contains(tuple.part) && where_clause(db, params, edge, tuple.part),
{
    let mapped = db.part.map_values(|part: PartRow| tuples_2(db, params, edge, part));
    lemma_flat_contains(mapped, tuple);
    if tuples_1(db, params, edge).contains(tuple) {
        let position = choose|position: int| 0 <= position < mapped.len() && (#[trigger] mapped[position]).contains(tuple);
        assert(tuple.part == db.part[position]);
        assert(db.part.contains(tuple.part));
    }
    if tuple.partsupp == edge && db.part.contains(tuple.part) && where_clause(db, params, edge, tuple.part) {
        let position = choose|position: int| 0 <= position < db.part.len() && #[trigger] db.part[position] == tuple.part;
        assert(mapped[position] =~= seq![tuple]);
        assert(mapped[position].len() == 1);
        assert(mapped[position][0] == tuple);
        assert(mapped[position].contains(tuple));
    }
}

pub proof fn lemma_tuple_members(db: Db, params: Params, tuple: Tuple)
    ensures tuples(db, params).contains(tuple) <==>
        db.partsupp.contains(tuple.partsupp) && db.part.contains(tuple.part)
            && where_clause(db, params, tuple.partsupp, tuple.part),
{
    let mapped = db.partsupp.map_values(|edge: PartsuppRow| tuples_1(db, params, edge));
    lemma_flat_contains(mapped, tuple);
    if tuples(db, params).contains(tuple) {
        let position = choose|position: int| 0 <= position < mapped.len() && (#[trigger] mapped[position]).contains(tuple);
        lemma_inner_members(db, params, db.partsupp[position], tuple);
        assert(db.partsupp.contains(tuple.partsupp));
    }
    if db.partsupp.contains(tuple.partsupp) && db.part.contains(tuple.part)
        && where_clause(db, params, tuple.partsupp, tuple.part) {
        let position = choose|position: int| 0 <= position < db.partsupp.len()
            && #[trigger] db.partsupp[position] == tuple.partsupp;
        lemma_inner_members(db, params, tuple.partsupp, tuple);
        assert(mapped[position].contains(tuple));
    }
}

pub proof fn lemma_support_nonneg(db: Db, supplier: int, brand: Seq<u8>, kind: Seq<u8>, size: int)
    ensures support(db, supplier, brand, kind, size) >= 0,
{
    assert forall|position: int| 0 <= position < db.part.len() implies
        support_term(db, supplier, brand, kind, size, #[trigger] db.part[position]) >= 0 by {
        lemma_sum_nonneg(db.partsupp, |row: PartsuppRow|
            if row.ps_partkey == db.part[position].p_partkey && row.ps_suppkey == supplier { 1int } else { 0int });
    }
    lemma_sum_nonneg(db.part, |row: PartRow| support_term(db, supplier, brand, kind, size, row));
}

pub proof fn lemma_tuple_support(db: Db, params: Params, tuple: Tuple)
    requires tuples(db, params).contains(tuple),
    ensures selected_group(params, tuple.part.p_brand, tuple.part.p_size),
        qualifying_supplier(db, tuple.part.p_brand, tuple.part.p_type, tuple.part.p_size, tuple.partsupp.ps_suppkey),
{
    lemma_tuple_members(db, params, tuple);
    let edge = choose|edge: int| 0 <= edge < db.partsupp.len() && #[trigger] db.partsupp[edge] == tuple.partsupp;
    let part = choose|part: int| 0 <= part < db.part.len() && #[trigger] db.part[part] == tuple.part;
    let edge_weight = |row: PartsuppRow|
        if row.ps_partkey == tuple.part.p_partkey && row.ps_suppkey == tuple.partsupp.ps_suppkey { 1int } else { 0int };
    lemma_sum_pos(db.partsupp, edge_weight, edge);
    let weight = |row: PartRow| support_term(db, tuple.partsupp.ps_suppkey,
        tuple.part.p_brand, tuple.part.p_type, tuple.part.p_size, row);
    assert forall|position: int| 0 <= position < db.part.len() implies #[trigger] weight(db.part[position]) >= 0 by {
        lemma_sum_nonneg(db.partsupp, |row: PartsuppRow|
            if row.ps_partkey == db.part[position].p_partkey && row.ps_suppkey == tuple.partsupp.ps_suppkey { 1int } else { 0int });
    }
    lemma_sum_pos(db.part, weight, part);
    lemma_blocked(db, params, tuple.partsupp.ps_suppkey);
}

pub proof fn lemma_support_tuple(db: Db, params: Params, supplier: int, brand: Seq<u8>, kind: Seq<u8>, size: int) -> (tuple: Tuple)
    requires selected_group(params, brand, size), qualifying_supplier(db, brand, kind, size, supplier),
    ensures tuples(db, params).contains(tuple), tuple.partsupp.ps_suppkey == supplier,
        tuple.part.p_brand == brand, tuple.part.p_type == kind, tuple.part.p_size == size,
{
    let weight = |row: PartRow| support_term(db, supplier, brand, kind, size, row);
    let part = lemma_sum_nonzero_witness(db.part, weight);
    let edge_weight = |row: PartsuppRow| if row.ps_partkey == db.part[part].p_partkey && row.ps_suppkey == supplier { 1int } else { 0int };
    lemma_sum_nonneg(db.partsupp, edge_weight);
    assert(edge_count(db, db.part[part].p_partkey, supplier) > 0);
    let edge = lemma_sum_nonzero_witness(db.partsupp, edge_weight);
    let tuple = Tuple { partsupp: db.partsupp[edge], part: db.part[part] };
    lemma_blocked(db, params, supplier);
    assert(where_clause(db, params, tuple.partsupp, tuple.part));
    assert(db.partsupp.contains(tuple.partsupp));
    assert(db.part.contains(tuple.part));
    lemma_tuple_members(db, params, tuple);
    tuple
}

pub proof fn lemma_group_values(db: Db, params: Params, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int)
    ensures group_vals(db, params, brand, kind, size, |tuple: Tuple| tuple.partsupp.ps_suppkey).contains(supplier)
        <==> selected_group(params, brand, size) && qualifying_supplier(db, brand, kind, size, supplier),
{
    let keep = |tuple: Tuple| tuple.part.p_brand == brand && tuple.part.p_type == kind && tuple.part.p_size == size;
    let project = |tuple: Tuple| tuple.partsupp.ps_suppkey;
    lemma_map_where(tuples(db, params), keep, project);
    if group_vals(db, params, brand, kind, size, project).contains(supplier) {
        let position = choose|position: int| 0 <= position < tuples(db, params).len()
            && keep(#[trigger] tuples(db, params)[position]) && project(tuples(db, params)[position]) == supplier;
        assert(tuples(db, params).contains(tuples(db, params)[position]));
        lemma_tuple_support(db, params, tuples(db, params)[position]);
    }
    if selected_group(params, brand, size) && qualifying_supplier(db, brand, kind, size, supplier) {
        let tuple = lemma_support_tuple(db, params, supplier, brand, kind, size);
        let position = choose|position: int| 0 <= position < tuples(db, params).len() && #[trigger] tuples(db, params)[position] == tuple;
        assert(keep(tuples(db, params)[position]) && project(tuples(db, params)[position]) == supplier);
    }
}

pub proof fn lemma_group_nonempty(db: Db, params: Params, brand: Seq<u8>, kind: Seq<u8>, size: int)
    ensures group_count(db, params, brand, kind, size) >= 0,
        (group_count(db, params, brand, kind, size) > 0) <==>
            distinct_count(group_vals(db, params, brand, kind, size, |tuple: Tuple| tuple.partsupp.ps_suppkey)) > 0,
{
    let keep = |tuple: Tuple| tuple.part.p_brand == brand && tuple.part.p_type == kind && tuple.part.p_size == size;
    let project = |tuple: Tuple| tuple.partsupp.ps_suppkey;
    let weight = |tuple: Tuple| if keep(tuple) { 1int } else { 0int };
    let raw = |tuple: Tuple| if tuple.part.p_brand == brand && tuple.part.p_type == kind && tuple.part.p_size == size { 1int } else { 0int };
    let values = group_vals(db, params, brand, kind, size, project);
    let keys = dedup(values);
    lemma_sum_ext_all(tuples(db, params), weight, raw);
    lemma_sum_nonneg(tuples(db, params), weight);
    lemma_map_where(tuples(db, params), keep, project);
    lemma_dedup(values);
    if group_count(db, params, brand, kind, size) > 0 {
        let position = lemma_sum_nonzero_witness(tuples(db, params), weight);
        let supplier = project(tuples(db, params)[position]);
        assert(values.contains(supplier));
        assert(keys.contains(supplier));
        assert(keys.len() > 0);
    }
    if keys.len() > 0 {
        assert(keys.contains(keys[0]));
        assert(values.contains(keys[0]));
        let position = choose|position: int| 0 <= position < tuples(db, params).len()
            && keep(#[trigger] tuples(db, params)[position]) && project(tuples(db, params)[position]) == keys[0];
        lemma_sum_pos(tuples(db, params), weight, position);
    }
}

pub proof fn lemma_query_total(db: Db, params: Params, brand: Seq<u8>, kind: Seq<u8>, size: int)
    ensures (group_count(db, params, brand, kind, size) > 0) <==>
        total(db, brand, kind, size) > 0 && selected_group(params, brand, size),
        row_of(db, params, brand, kind, size).supplier_cnt ==
            if selected_group(params, brand, size) { total(db, brand, kind, size) } else { 0int },
{
    let project = |tuple: Tuple| tuple.partsupp.ps_suppkey;
    let values = group_vals(db, params, brand, kind, size, project);
    let qualified = qualified_keys(db.partsupp, db, brand, kind, size);
    let left = dedup(values);
    let right = dedup(qualified);
    lemma_dedup(values);
    lemma_dedup(qualified);
    lemma_group_nonempty(db, params, brand, kind, size);
    if selected_group(params, brand, size) {
        assert forall|supplier: int| #[trigger] left.contains(supplier) <==> right.contains(supplier) by {
            lemma_group_values(db, params, brand, kind, size, supplier);
            crate::build_semantics::lemma_qualified_members(db.partsupp, db, brand, kind, size, supplier);
            if qualifying_supplier(db, brand, kind, size, supplier) {
                crate::build_semantics::lemma_qualified_witness(db, brand, kind, size, supplier);
            }
        }
        assert(left.to_set() =~= right.to_set());
        assert(left.no_duplicates()) by {
            assert forall|first: int, last: int| 0 <= first < left.len() && 0 <= last < left.len() && first != last
                implies (#[trigger] left[first]) != (#[trigger] left[last]) by {
                if last < first { assert(left[last] != left[first]); }
            }
        }
        assert(right.no_duplicates()) by {
            assert forall|first: int, last: int| 0 <= first < right.len() && 0 <= last < right.len() && first != last
                implies (#[trigger] right[first]) != (#[trigger] right[last]) by {
                if last < first { assert(right[last] != right[first]); }
            }
        }
        verus_prooflib::lemma_sum_same_support(left, right, |supplier: int| 1int);
        verus_prooflib::lemma_sum_ones(left);
        verus_prooflib::lemma_sum_ones(right);
    } else {
        if left.len() > 0 {
            assert(left.contains(left[0]));
            assert(values.contains(left[0]));
            lemma_group_values(db, params, brand, kind, size, left[0]);
            assert(false);
        }
    }
}

pub proof fn lemma_query_bridge(db: Db, params: Params)
    ensures
        forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            #[trigger] group_count(db, params, brand, kind, size) > 0 <==>
                total(db, brand, kind, size) > 0 && selected_group(params, brand, size),
        forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            #[trigger] group_count(db, params, brand, kind, size) > 0 ==>
                row_of(db, params, brand, kind, size).supplier_cnt == total(db, brand, kind, size),
{
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] group_count(db, params, brand, kind, size) > 0 <==>
            total(db, brand, kind, size) > 0 && selected_group(params, brand, size) by {
        lemma_query_total(db, params, brand, kind, size);
    }
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] group_count(db, params, brand, kind, size) > 0 implies
            row_of(db, params, brand, kind, size).supplier_cnt == total(db, brand, kind, size) by {
        lemma_query_total(db, params, brand, kind, size);
    }
}

}
