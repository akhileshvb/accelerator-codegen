// The rollup change of a transaction: the lines of the rescans in both worlds, and the changed lines in the old one.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::events::*;
use crate::orders::*;
use crate::rollup::*;
use crate::applyspec::*;
use crate::yw::*;
use crate::dimensions::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_contains_iff(keys: Seq<i64>, k: i64)
    ensures keys.contains(k) <==> seq_view_i64(keys).contains(k as int),
{
    if keys.contains(k) { lemma_view_contains(keys, k); }
    if seq_view_i64(keys).contains(k as int) {
        lemma_view_contains_inv(keys, k as int);
        let k2 = choose|k2: i64| keys.contains(k2) && k2 as int == k as int;
        assert(k2 == k);
    }
}

pub fn collect_okeys(changes: &[OrderDelta]) -> (r: Vec<i64>)
    ensures r@ == okeys_of(changes@),
{
    let mut r: Vec<i64> = Vec::new();
    let mut i = 0;
    while i < changes.len()
        invariant i <= changes.len(), r@ == okeys_of(changes@).take(i as int),
        decreases changes.len() - i,
    { r.push(changes[i].row.o_orderkey); i += 1; }
    proof { assert(okeys_of(changes@).take(i as int) =~= okeys_of(changes@)); }
    r
}

pub fn collect_pkeys(part: &Vec<PartDelta>, ps: &[PartsuppDelta]) -> (r: Vec<i64>)
    ensures forall|k: i64| #[trigger] r@.contains(k) <==> (part_keys_of(part@).contains(k) || ps_keys_of(ps@).contains(k)),
{
    let mut r: Vec<i64> = Vec::new();
    let mut i = 0;
    while i < part.len()
        invariant i <= part.len(), r@ == part_keys_of(part@).take(i as int),
        decreases part.len() - i,
    { r.push(part[i].row.p_partkey); i += 1; }
    proof { assert(part_keys_of(part@).take(i as int) =~= part_keys_of(part@)); }
    let mut j = 0;
    while j < ps.len()
        invariant j <= ps.len(), r@ == part_keys_of(part@) + ps_keys_of(ps@).take(j as int),
        decreases ps.len() - j,
    {
        r.push(ps[j].row.ps_partkey);
        proof { assert(r@ =~= part_keys_of(part@) + ps_keys_of(ps@).take(j as int + 1)); }
        j += 1;
    }
    proof {
        assert(ps_keys_of(ps@).take(j as int) =~= ps_keys_of(ps@));
        assert forall|k: i64| #[trigger] r@.contains(k) <==> (part_keys_of(part@).contains(k) || ps_keys_of(ps@).contains(k)) by {
            if r@.contains(k) {
                let q = choose|q: int| 0 <= q < r@.len() && r@[q] == k;
                if q < part_keys_of(part@).len() { assert(part_keys_of(part@)[q] == k); }
                else { assert(ps_keys_of(ps@)[q - part_keys_of(part@).len()] == k); }
            }
            if part_keys_of(part@).contains(k) {
                let q = choose|q: int| 0 <= q < part_keys_of(part@).len() && part_keys_of(part@)[q] == k;
                assert(r@[q] == k);
            }
            if ps_keys_of(ps@).contains(k) {
                let q = choose|q: int| 0 <= q < ps_keys_of(ps@).len() && ps_keys_of(ps@)[q] == k;
                assert(r@[part_keys_of(part@).len() + q] == k);
            }
        }
    }
    r
}

pub fn is_cancel_exec(sup: &[SupplierDelta]) -> (r: bool)
    ensures r == is_cancel(sup@),
{
    sup.len() == 2 && sup[0].row.s_suppkey == sup[1].row.s_suppkey && sup[0].row.s_nationkey == sup[1].row.s_nationkey
        && (sup[0].sign as i128) + (sup[1].sign as i128) == 0
}

pub fn collect_skeys(sup: &[SupplierDelta]) -> (r: Vec<i64>)
    ensures forall|k: i64| #[trigger] r@.contains(k) <==> (!is_cancel(sup@) && sup_keys_of(sup@).contains(k)),
{
    let mut r: Vec<i64> = Vec::new();
    if is_cancel_exec(sup) {
        proof { assert forall|k: i64| !r@.contains(k) by {} }
        return r;
    }
    let mut i = 0;
    while i < sup.len()
        invariant i <= sup.len(), r@ == sup_keys_of(sup@).take(i as int),
        decreases sup.len() - i,
    { r.push(sup[i].row.s_suppkey); i += 1; }
    proof { assert(sup_keys_of(sup@).take(i as int) =~= sup_keys_of(sup@)); }
    r
}

pub fn collect_pskeys(ps: &[PartsuppDelta]) -> (r: Vec<i64>)
    ensures r@ == ps_keys_of(ps@),
{
    let mut r: Vec<i64> = Vec::new();
    let mut i = 0;
    while i < ps.len()
        invariant i <= ps.len(), r@ == ps_keys_of(ps@).take(i as int),
        decreases ps.len() - i,
    { r.push(ps[i].row.ps_partkey); i += 1; }
    proof { assert(ps_keys_of(ps@).take(i as int) =~= ps_keys_of(ps@)); }
    r
}

pub open spec fn mode_weight(db: Db, db2: Db, p: Seq<u8>, nk: int, year: int, field: int, mode: int, okeys: Seq<i64>, pkeys: Seq<i64>, skeys: Seq<i64>) -> spec_fn(LineIn) -> int {
    gsel_in(mode, okeys, pkeys, skeys, gdiff(db, db2, p, nk, year, field))
}

/// The rows of one rescan that the mode selects, in both worlds.
#[verifier::spinoff_prover]
pub fn diff_rows(rows: &[LineIn], mode: u8, okk: &Vec<i64>, pkk: &Vec<i64>, skk: &Vec<i64>,
    post: &Work<Vec<Year>>, chg: &Work<Vec<Year>>, ords: &[i64], orders_delta: &[OrderDelta],
    m1: &Pages<Vec<MaskRow>>, p1: &Pages<Vec<Pair>>, s1: &Pages<Vec<Number>>,
    m2: &Pages<Vec<MaskRow>>, p2: &Pages<Vec<Pair>>, s2: &Pages<Vec<Number>>,
    n: usize, delta: &mut Vec<Vec<RCell>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(pv): Ghost<Seq<Seq<u8>>>) -> (r: Option<()>)
    requires masks_ready(*m1, db, pv), pairs_ready(*p1, db), supplier_pages_ready(*s1, db),
        masks_ready(*m2, db2, pv), pairs_ready(*p2, db2), supplier_pages_ready(*s2, db2),
        post_dates_ready(*post, ords@, db2), order_changes_ready(*chg, orders_delta@),
        bag_step(db.orders, db2.orders, order_delta_seq_view(orders_delta@)),
        n == pv.len(), n <= 127, old(delta).len() == n, mode <= 2,
    ensures r.is_some() ==> final(delta).len() == n && forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 ==> #[trigger] dval(final(delta)@, c, nk, y, f)
        == dval(old(delta)@, c, nk, y, f) + sum(rows@, mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@)),
{
    let ghost old_delta = delta@;
    proof {
        assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c, nk, y, f)
            == dval(old_delta, c, nk, y, f) + sum(rows@.take(0), mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@)) by {
            lemma_sum_empty(rows@.take(0), mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@));
        }
    }
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), delta.len() == n, n == pv.len(), n <= 127, mode <= 2,
            masks_ready(*m1, db, pv), pairs_ready(*p1, db), supplier_pages_ready(*s1, db),
            masks_ready(*m2, db2, pv), pairs_ready(*p2, db2), supplier_pages_ready(*s2, db2),
            post_dates_ready(*post, ords@, db2), order_changes_ready(*chg, orders_delta@),
            bag_step(db.orders, db2.orders, order_delta_seq_view(orders_delta@)),
            forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 ==> #[trigger] dval(delta@, c, nk, y, f)
                == dval(old_delta, c, nk, y, f) + sum(rows@.take(index as int), mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@)),
        decreases rows.len() - index,
    {
        let line = rows[index];
        let in_o = has_key(okk, line.l_orderkey);
        let in_p = has_key(pkk, line.l_partkey);
        let in_s = has_key(skk, line.l_suppkey);
        let selected = if mode == 0 { in_o } else if mode == 1 { !in_o && in_p } else { !in_o && !in_p && in_s };
        proof {
            lemma_contains_iff(okk@, line.l_orderkey);
            lemma_contains_iff(pkk@, line.l_partkey);
            lemma_contains_iff(skk@, line.l_suppkey);
            assert(selected == selr(mode as int, okk@, pkk@, skk@, line_in_view(line)));
        }
        let ghost before = delta@;
        if selected {
            let (w1, w2) = year_ws(post, chg, line.l_orderkey, ords, orders_delta, Ghost(db), Ghost(db2))?;
            let ghost mid;
            line_world(m2, p2, s2, n, line, &w2, 1, delta, Ghost(db2), Ghost(pv))?;
            proof { mid = delta@; }
            line_world(m1, p1, s1, n, line, &w1, -1, delta, Ghost(db), Ghost(pv))?;
            proof {
                assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c, nk, y, f)
                    == dval(old_delta, c, nk, y, f) + sum(rows@.take(index as int + 1), mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@)) by {
                    lemma_sum_take(rows@, index as int, mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@));
                    assert(dval(mid, c, nk, y, f) == dval(before, c, nk, y, f) + 1 * hline(db2, pv[c], line_in_view(line), nk, y, f));
                    assert(dval(delta@, c, nk, y, f) == dval(mid, c, nk, y, f) + (-1int) * hline(db, pv[c], line_in_view(line), nk, y, f));
                }
            }
        } else {
            proof {
                assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c, nk, y, f)
                    == dval(old_delta, c, nk, y, f) + sum(rows@.take(index as int + 1), mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@)) by {
                    lemma_sum_take(rows@, index as int, mode_weight(db, db2, pv[c], nk, y, f, mode as int, okk@, pkk@, skk@));
                }
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(())
}

pub open spec fn ldelta_weight(db: Db, p: Seq<u8>, nk: int, year: int, field: int) -> spec_fn(LineDelta) -> int {
    |d: LineDelta| d.sign as int * hline(db, p, line_in_view(d.row), nk, year, field)
}

/// The changed lines, weighted by sign, in the old world.
#[verifier::spinoff_prover]
pub fn delta_lines(lines: &[LineDelta], post: &Work<Vec<Year>>, chg: &Work<Vec<Year>>, ords: &[i64], orders_delta: &[OrderDelta],
    m1: &Pages<Vec<MaskRow>>, p1: &Pages<Vec<Pair>>, s1: &Pages<Vec<Number>>,
    n: usize, delta: &mut Vec<Vec<RCell>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(pv): Ghost<Seq<Seq<u8>>>) -> (r: Option<()>)
    requires masks_ready(*m1, db, pv), pairs_ready(*p1, db), supplier_pages_ready(*s1, db),
        post_dates_ready(*post, ords@, db2), order_changes_ready(*chg, orders_delta@),
        bag_step(db.orders, db2.orders, order_delta_seq_view(orders_delta@)),
        n == pv.len(), n <= 127, old(delta).len() == n,
    ensures r.is_some() ==> final(delta).len() == n && forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 ==> #[trigger] dval(final(delta)@, c, nk, y, f)
        == dval(old(delta)@, c, nk, y, f) + sum(lines@, ldelta_weight(db, pv[c], nk, y, f)),
{
    let ghost old_delta = delta@;
    proof {
        assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c, nk, y, f)
            == dval(old_delta, c, nk, y, f) + sum(lines@.take(0), ldelta_weight(db, pv[c], nk, y, f)) by {
            lemma_sum_empty(lines@.take(0), ldelta_weight(db, pv[c], nk, y, f));
        }
    }
    let mut index = 0;
    while index < lines.len()
        invariant index <= lines.len(), delta.len() == n, n == pv.len(), n <= 127,
            masks_ready(*m1, db, pv), pairs_ready(*p1, db), supplier_pages_ready(*s1, db),
            post_dates_ready(*post, ords@, db2), order_changes_ready(*chg, orders_delta@),
            bag_step(db.orders, db2.orders, order_delta_seq_view(orders_delta@)),
            forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 ==> #[trigger] dval(delta@, c, nk, y, f)
                == dval(old_delta, c, nk, y, f) + sum(lines@.take(index as int), ldelta_weight(db, pv[c], nk, y, f)),
        decreases lines.len() - index,
    {
        let d = lines[index];
        let (w1, w2) = year_ws(post, chg, d.row.l_orderkey, ords, orders_delta, Ghost(db), Ghost(db2))?;
        let ghost before = delta@;
        line_world(m1, p1, s1, n, d.row, &w1, d.sign as i128, delta, Ghost(db), Ghost(pv))?;
        proof {
            assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c, nk, y, f)
                == dval(old_delta, c, nk, y, f) + sum(lines@.take(index as int + 1), ldelta_weight(db, pv[c], nk, y, f)) by {
                lemma_sum_take(lines@, index as int, ldelta_weight(db, pv[c], nk, y, f));
            }
        }
        index += 1;
    }
    proof { assert(lines@.take(index as int) =~= lines@); }
    Some(())
}

}
