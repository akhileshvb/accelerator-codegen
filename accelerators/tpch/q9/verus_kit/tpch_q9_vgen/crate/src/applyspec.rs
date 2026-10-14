// What a transaction leaves unchanged, and how the changed lines are found.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::rollup::*;
use crate::maskupdate::*;
use crate::pairupdate::*;
use crate::dimensions::*;
use crate::query_proofs::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn okeys_of(changes: Seq<OrderDelta>) -> Seq<i64> {
    changes.map_values(|d: OrderDelta| d.row.o_orderkey)
}

pub open spec fn part_keys_of(changes: Seq<PartDelta>) -> Seq<i64> {
    changes.map_values(|d: PartDelta| d.row.p_partkey)
}

pub open spec fn ps_keys_of(changes: Seq<PartsuppDelta>) -> Seq<i64> {
    changes.map_values(|d: PartsuppDelta| d.row.ps_partkey)
}

pub open spec fn is_cancel(ch: Seq<SupplierDelta>) -> bool {
    ch.len() == 2 && ch[0].row.s_suppkey == ch[1].row.s_suppkey && ch[0].row.s_nationkey == ch[1].row.s_nationkey && ch[0].sign + ch[1].sign == 0
}

pub open spec fn sup_keys_of(ch: Seq<SupplierDelta>) -> Seq<i64> {
    ch.map_values(|d: SupplierDelta| d.row.s_suppkey)
}

pub open spec fn selr(mode: int, okeys: Seq<i64>, pkeys: Seq<i64>, skeys: Seq<i64>, l: LineitemRow) -> bool {
    if mode == 0 { seq_view_i64(okeys).contains(l.l_orderkey) }
    else if mode == 1 { !seq_view_i64(okeys).contains(l.l_orderkey) && seq_view_i64(pkeys).contains(l.l_partkey) }
    else { !seq_view_i64(okeys).contains(l.l_orderkey) && !seq_view_i64(pkeys).contains(l.l_partkey) && seq_view_i64(skeys).contains(l.l_suppkey) }
}

pub open spec fn gdiff(db: Db, db2: Db, p: Seq<u8>, nk: int, year: int, field: int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| hline(db2, p, l, nk, year, field) - hline(db, p, l, nk, year, field)
}

pub open spec fn gsel(mode: int, okeys: Seq<i64>, pkeys: Seq<i64>, skeys: Seq<i64>, g: spec_fn(LineitemRow) -> int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| if selr(mode, okeys, pkeys, skeys, l) { g(l) } else { 0int }
}

pub open spec fn gsel_in(mode: int, okeys: Seq<i64>, pkeys: Seq<i64>, skeys: Seq<i64>, g: spec_fn(LineitemRow) -> int) -> spec_fn(LineIn) -> int {
    |r: LineIn| gsel(mode, okeys, pkeys, skeys, g)(line_in_view(r))
}

pub proof fn lemma_view_contains(keys: Seq<i64>, k: i64)
    requires keys.contains(k),
    ensures seq_view_i64(keys).contains(k as int),
{
    let j = choose|j: int| 0 <= j < keys.len() && keys[j] == k;
    assert(seq_view_i64(keys)[j] == k as int);
}

pub proof fn lemma_view_contains_inv(keys: Seq<i64>, v: int)
    requires seq_view_i64(keys).contains(v),
    ensures exists|k: i64| keys.contains(k) && k as int == v,
{
    let j = choose|j: int| 0 <= j < seq_view_i64(keys).len() && seq_view_i64(keys)[j] == v;
    assert(keys[j] as int == v);
    assert(keys.contains(keys[j]));
}

pub proof fn lemma_dates_frame(db: Db, db2: Db, changes: Seq<OrderDelta>, key: int, year: int)
    requires bag_step(db.orders, db2.orders, order_delta_seq_view(changes)), !seq_view_i64(okeys_of(changes)).contains(key),
    ensures dates(db2, key, year) == dates(db, key, year),
{
    let w = |row: OrdersRow| if row.o_orderkey == key && civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let signed = |entry: (OrdersRow, int)| entry.1 * w(entry.0);
    assert(sum(db2.orders, w) == sum(db.orders, w) + sum(order_delta_seq_view(changes), signed));
    assert forall|i: int| 0 <= i < order_delta_seq_view(changes).len() implies signed(#[trigger] order_delta_seq_view(changes)[i]) == 0 by {
        assert(seq_view_i64(okeys_of(changes))[i] == changes[i].row.o_orderkey as int);
        if changes[i].row.o_orderkey as int == key { assert(seq_view_i64(okeys_of(changes)).contains(key)); }
        assert(order_delta_seq_view(changes)[i].1 * 0int == 0) by (nonlinear_arith);
    }
    lemma_sum_zero(order_delta_seq_view(changes), signed);
}

pub proof fn lemma_green_frame(db: Db, db2: Db, changes: Seq<PartDelta>, p: Seq<u8>, key: int)
    requires bag_step(db.part, db2.part, part_delta_seq_view(changes)), !seq_view_i64(part_keys_of(changes)).contains(key),
    ensures green(filter_db(db2, p), key) == green(filter_db(db, p), key),
{
    let w = matched_weight(p, key);
    let signed = |entry: (PartRow, int)| entry.1 * w(entry.0);
    lemma_matched_green(db, p, key);
    lemma_matched_green(db2, p, key);
    assert(sum(db2.part, w) == sum(db.part, w) + sum(part_delta_seq_view(changes), signed));
    assert forall|i: int| 0 <= i < part_delta_seq_view(changes).len() implies signed(#[trigger] part_delta_seq_view(changes)[i]) == 0 by {
        assert(seq_view_i64(part_keys_of(changes))[i] == changes[i].row.p_partkey as int);
        if changes[i].row.p_partkey as int == key { assert(seq_view_i64(part_keys_of(changes)).contains(key)); }
        assert(part_delta_seq_view(changes)[i].1 * 0int == 0) by (nonlinear_arith);
    }
    lemma_sum_zero(part_delta_seq_view(changes), signed);
}

pub proof fn lemma_supplier_frame(db: Db, db2: Db, changes: Seq<SupplierDelta>, key: int, nation: int)
    requires bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(changes)),
        is_cancel(changes) || !seq_view_i64(sup_keys_of(changes)).contains(key),
    ensures supplier_count(db2, key, nation) == supplier_count(db, key, nation),
{
    let w = |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation { 1int } else { 0int };
    let signed = |entry: (SupplierRow, int)| entry.1 * w(entry.0);
    let view = supplier_delta_seq_view(changes);
    assert(sum(db2.supplier, w) == sum(db.supplier, w) + sum(view, signed));
    if is_cancel(changes) {
        assert(view.len() == 2);
        assert(view[0].0 == view[1].0);
        assert(view[0].1 + view[1].1 == 0);
        let e = Seq::<(SupplierRow, int)>::empty();
        assert(view =~= e.push(view[0]).push(view[1]));
        lemma_sum_empty(e, signed);
        lemma_sum_push(e, view[0], signed);
        lemma_sum_push(e.push(view[0]), view[1], signed);
        let x = w(view[0].0);
        let s0 = view[0].1;
        let s1 = view[1].1;
        assert(signed(view[0]) == s0 * x);
        assert(signed(view[1]) == s1 * x);
        assert(s0 * x + s1 * x == (s0 + s1) * x) by (nonlinear_arith);
        assert((s0 + s1) * x == 0) by (nonlinear_arith) requires s0 + s1 == 0;
    } else {
        assert forall|i: int| 0 <= i < view.len() implies signed(#[trigger] view[i]) == 0 by {
            assert(seq_view_i64(sup_keys_of(changes))[i] == changes[i].row.s_suppkey as int);
            if changes[i].row.s_suppkey as int == key { assert(seq_view_i64(sup_keys_of(changes)).contains(key)); }
            assert(view[i].1 * 0int == 0) by (nonlinear_arith);
        }
        lemma_sum_zero(view, signed);
    }
}

pub proof fn lemma_line_frame(db: Db, db2: Db, p: Seq<u8>, l: LineitemRow, nk: int, y: int, f: int,
    orders: Seq<OrderDelta>, part: Seq<PartDelta>, ps: Seq<PartsuppDelta>, sup: Seq<SupplierDelta>,
    pkeys: Seq<i64>, skeys: Seq<i64>)
    requires bag_step(db.orders, db2.orders, order_delta_seq_view(orders)),
        bag_step(db.part, db2.part, part_delta_seq_view(part)),
        bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(ps)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(sup)),
        0 <= f <= 1,
        forall|k: i64| #[trigger] pkeys.contains(k) <==> (part_keys_of(part).contains(k) || ps_keys_of(ps).contains(k)),
        forall|k: i64| #[trigger] skeys.contains(k) <==> (!is_cancel(sup) && sup_keys_of(sup).contains(k)),
        !seq_view_i64(okeys_of(orders)).contains(l.l_orderkey),
        !seq_view_i64(pkeys).contains(l.l_partkey),
        !seq_view_i64(skeys).contains(l.l_suppkey),
    ensures hline(db2, p, l, nk, y, f) == hline(db, p, l, nk, y, f),
{
    lemma_dates_frame(db, db2, orders, l.l_orderkey, y);
    assert(!seq_view_i64(part_keys_of(part)).contains(l.l_partkey)) by {
        if seq_view_i64(part_keys_of(part)).contains(l.l_partkey) {
            let i = choose|i: int| 0 <= i < seq_view_i64(part_keys_of(part)).len() && seq_view_i64(part_keys_of(part))[i] == l.l_partkey;
            assert(part_keys_of(part)[i] as int == l.l_partkey);
            assert(part_keys_of(part).contains(part_keys_of(part)[i]));
            assert(pkeys.contains(part_keys_of(part)[i]));
            lemma_view_contains(pkeys, part_keys_of(part)[i]);
        }
    }
    lemma_green_frame(db, db2, part, p, l.l_partkey);
    assert forall|i: int| 0 <= i < ps.len() implies (#[trigger] ps[i]).row.ps_partkey as int != l.l_partkey by {
        if ps[i].row.ps_partkey as int == l.l_partkey {
            assert(ps_keys_of(ps)[i] == ps[i].row.ps_partkey);
            assert(ps_keys_of(ps).contains(ps[i].row.ps_partkey));
            assert(pkeys.contains(ps[i].row.ps_partkey));
            lemma_view_contains(pkeys, ps[i].row.ps_partkey);
        }
    }
    lemma_pair_frame(db, db2, ps, l.l_partkey, l.l_suppkey, 0);
    lemma_pair_frame(db, db2, ps, l.l_partkey, l.l_suppkey, 1);
    assert(!is_cancel(sup) ==> !seq_view_i64(sup_keys_of(sup)).contains(l.l_suppkey)) by {
        if !is_cancel(sup) && seq_view_i64(sup_keys_of(sup)).contains(l.l_suppkey) {
            let i = choose|i: int| 0 <= i < seq_view_i64(sup_keys_of(sup)).len() && seq_view_i64(sup_keys_of(sup))[i] == l.l_suppkey;
            assert(sup_keys_of(sup)[i] as int == l.l_suppkey);
            assert(sup_keys_of(sup).contains(sup_keys_of(sup)[i]));
            assert(skeys.contains(sup_keys_of(sup)[i]));
            lemma_view_contains(skeys, sup_keys_of(sup)[i]);
        }
    }
    assert forall|nation: int| supplier_count(db2, l.l_suppkey, nation) == supplier_count(db, l.l_suppkey, nation) by {
        lemma_supplier_frame(db, db2, sup, l.l_suppkey, nation);
    }
}

/// Lines selected by none of the three modes are unaffected, so a sum over db2's lines splits over the rescans.
pub proof fn lemma_decompose(db2: Db, okeys: Seq<i64>, pkeys: Seq<i64>, skeys: Seq<i64>,
    keys_a: Seq<i64>, rows_a: Seq<LineIn>, keys_b: Seq<i64>, rows_b: Seq<LineIn>, keys_c: Seq<i64>, rows_c: Seq<LineIn>,
    g: spec_fn(LineitemRow) -> int)
    requires rescan_lines_for_orders_ok(db2, keys_a, rows_a), rescan_lines_for_parts_ok(db2, keys_b, rows_b), rescan_lines_for_suppliers_ok(db2, keys_c, rows_c),
        forall|k: i64| okeys.contains(k) ==> #[trigger] keys_a.contains(k),
        forall|k: i64| pkeys.contains(k) ==> #[trigger] keys_b.contains(k),
        forall|k: i64| skeys.contains(k) ==> #[trigger] keys_c.contains(k),
        forall|l: LineitemRow| !selr(0, okeys, pkeys, skeys, l) && !selr(1, okeys, pkeys, skeys, l) && !selr(2, okeys, pkeys, skeys, l) ==> g(l) == 0,
    ensures sum(db2.lineitem, g) == sum(rows_a, gsel_in(0, okeys, pkeys, skeys, g)) + sum(rows_b, gsel_in(1, okeys, pkeys, skeys, g)) + sum(rows_c, gsel_in(2, okeys, pkeys, skeys, g)),
{
    let ga = gsel(0, okeys, pkeys, skeys, g);
    let gb = gsel(1, okeys, pkeys, skeys, g);
    let gc = gsel(2, okeys, pkeys, skeys, g);
    let gab = |l: LineitemRow| ga(l) + gb(l);
    assert forall|l: LineitemRow| #[trigger] g(l) == gab(l) + gc(l) by {}
    lemma_sum_add(db2.lineitem, ga, gb, gab);
    lemma_sum_add(db2.lineitem, gab, gc, g);
    // each part through its rescan
    let fa = |l: LineitemRow| if seq_view_i64(keys_a).contains(l.l_orderkey) { ga(l) } else { 0int };
    assert forall|l: LineitemRow| #[trigger] fa(l) == ga(l) by {
        if selr(0, okeys, pkeys, skeys, l) {
            assert(keys_a.contains(l.l_orderkey as i64) || true);
            let i = choose|i: int| 0 <= i < seq_view_i64(okeys).len() && seq_view_i64(okeys)[i] == l.l_orderkey;
            assert(okeys[i] as int == l.l_orderkey);
            assert(keys_a.contains(okeys[i]));
            let j = choose|j: int| 0 <= j < keys_a.len() && keys_a[j] == okeys[i];
            assert(seq_view_i64(keys_a)[j] == l.l_orderkey);
        }
    }
    lemma_sum_ext_all(db2.lineitem, fa, ga);
    lemma_sum_map(rows_a, |r: LineIn| line_in_view(r), ga, gsel_in(0, okeys, pkeys, skeys, g));
    assert(sum(line_in_seq_view(rows_a), ga) == sum(db2.lineitem, fa));
    let fb = |l: LineitemRow| if seq_view_i64(keys_b).contains(l.l_partkey) { gb(l) } else { 0int };
    assert forall|l: LineitemRow| #[trigger] fb(l) == gb(l) by {
        if selr(1, okeys, pkeys, skeys, l) {
            let i = choose|i: int| 0 <= i < seq_view_i64(pkeys).len() && seq_view_i64(pkeys)[i] == l.l_partkey;
            assert(pkeys[i] as int == l.l_partkey);
            assert(keys_b.contains(pkeys[i]));
            let j = choose|j: int| 0 <= j < keys_b.len() && keys_b[j] == pkeys[i];
            assert(seq_view_i64(keys_b)[j] == l.l_partkey);
        }
    }
    lemma_sum_ext_all(db2.lineitem, fb, gb);
    lemma_sum_map(rows_b, |r: LineIn| line_in_view(r), gb, gsel_in(1, okeys, pkeys, skeys, g));
    assert(sum(line_in_seq_view(rows_b), gb) == sum(db2.lineitem, fb));
    let fc = |l: LineitemRow| if seq_view_i64(keys_c).contains(l.l_suppkey) { gc(l) } else { 0int };
    assert forall|l: LineitemRow| #[trigger] fc(l) == gc(l) by {
        if selr(2, okeys, pkeys, skeys, l) {
            let i = choose|i: int| 0 <= i < seq_view_i64(skeys).len() && seq_view_i64(skeys)[i] == l.l_suppkey;
            assert(skeys[i] as int == l.l_suppkey);
            assert(keys_c.contains(skeys[i]));
            let j = choose|j: int| 0 <= j < keys_c.len() && keys_c[j] == skeys[i];
            assert(seq_view_i64(keys_c)[j] == l.l_suppkey);
        }
    }
    lemma_sum_ext_all(db2.lineitem, fc, gc);
    lemma_sum_map(rows_c, |r: LineIn| line_in_view(r), gc, gsel_in(2, okeys, pkeys, skeys, g));
    assert(sum(line_in_seq_view(rows_c), gc) == sum(db2.lineitem, fc));
}

}
