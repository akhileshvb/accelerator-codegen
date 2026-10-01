// Proof vocabulary for the Q1 kernel (not trusted): the accumulator-level
// group sums the kernel maintains, and their link to the generated spec
// (gen_spec.rs).
//
// The kernel's rows are in the host's day encoding and its date bound is a
// host-computed `cutoff` in that encoding.  For a host whose day numbers are
// days since 1970-01-01 minus e (0 for DuckDB, 10957 for PostgreSQL DateADT,
// -719528 for MySQL TO_DAYS), the generated spec sees the rows shifted by e,
// and $1 = d with cutoff + e = 10561 - d (10561 is DATE '1998-12-01').
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;

verus! {

/// WHERE l_shipdate <= cutoff, in the host's encoding.
pub open spec fn qualifies(l: LineitemRow, cutoff: int) -> bool {
    l.l_shipdate <= cutoff
}

pub open spec fn in_group(l: LineitemRow, cutoff: int, rf: Seq<u8>, ls: Seq<u8>) -> bool {
    qualifies(l, cutoff) && l.l_returnflag == rf && l.l_linestatus == ls
}

/// l_extendedprice * (1 - l_discount), in units of 10^-4.
pub open spec fn disc_price(l: LineitemRow) -> int {
    l.l_extendedprice * (100 - l.l_discount)
}

/// l_extendedprice * (1 - l_discount) * (1 + l_tax), in units of 10^-6.
pub open spec fn charge(l: LineitemRow) -> int {
    l.l_extendedprice * (100 - l.l_discount) * (100 + l.l_tax)
}

pub open spec fn f_qty() -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| l.l_quantity
}

pub open spec fn f_base() -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| l.l_extendedprice
}

pub open spec fn f_disc_price() -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| disc_price(l)
}

pub open spec fn f_charge() -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| charge(l)
}

pub open spec fn f_disc() -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| l.l_discount
}

pub open spec fn f_one() -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| 1int
}

/// Σ f(l) over the rows of group (rf, ls) that pass the WHERE clause.
pub open spec fn gsum(
    db: Seq<LineitemRow>,
    cutoff: int,
    rf: Seq<u8>,
    ls: Seq<u8>,
    f: spec_fn(LineitemRow) -> int,
) -> int {
    sum(db, |l: LineitemRow| if in_group(l, cutoff, rf, ls) { f(l) } else { 0 })
}

// ── Day encodings ──────────────────────────────────────────────────────────
pub open spec fn shift_row(l: LineitemRow, e: int) -> LineitemRow {
    LineitemRow { l_shipdate: l.l_shipdate + e, ..l }
}

/// The table in days since 1970-01-01, for rows in an encoding offset by e.
pub open spec fn db_at(rows: Seq<LineitemRow>, e: int) -> Db {
    Db { lineitem: rows.map_values(|l: LineitemRow| shift_row(l, e)) }
}

pub open spec fn params_of(d: int) -> Params {
    Params { p1: d }
}

/// A kernel group sum is the generated group_sum, for summands that agree.
pub proof fn lemma_gsum_eq(
    rows: Seq<LineitemRow>,
    e: int,
    cutoff: int,
    d: int,
    rf: Seq<u8>,
    ls: Seq<u8>,
    hf: spec_fn(LineitemRow) -> int,
    f: spec_fn(Tuple) -> int,
)
    requires
        cutoff + e == 10561 - d,
        forall|l: LineitemRow| #[trigger] hf(l) == f(Tuple { lineitem: shift_row(l, e) }),
    ensures
        gsum(rows, cutoff, rf, ls, hf) == group_sum(db_at(rows, e), params_of(d), rf, ls, f),
{
    let gd = db_at(rows, e);
    let ff = |t: Tuple| if t.lineitem.l_returnflag == rf && t.lineitem.l_linestatus == ls { f(t) } else { 0 };
    assert forall|t: Tuple| #[trigger] ff(t) == if t.lineitem.l_returnflag == rf && t.lineitem.l_linestatus == ls { hf(Lineitem_unshift(t.lineitem, e)) } else { 0 } by {
        let l = Lineitem_unshift(t.lineitem, e);
        assert(shift_row(l, e) == t.lineitem);
        assert(t == Tuple { lineitem: t.lineitem });
        assert(hf(l) == f(Tuple { lineitem: shift_row(l, e) }));
    }
    lemma_tuples_sum(rows, e, cutoff, d, rf, ls, hf, ff);
}

/// The kernel's group count is the generated group_count.
pub proof fn lemma_gcount_eq(rows: Seq<LineitemRow>, e: int, cutoff: int, d: int, rf: Seq<u8>, ls: Seq<u8>)
    requires
        cutoff + e == 10561 - d,
    ensures
        gsum(rows, cutoff, rf, ls, f_one()) == group_count(db_at(rows, e), params_of(d), rf, ls),
{
    let ff = |t: Tuple| if t.lineitem.l_returnflag == rf && t.lineitem.l_linestatus == ls { 1int } else { 0 };
    lemma_tuples_sum(rows, e, cutoff, d, rf, ls, f_one(), ff);
}

/// Σ over the generated tuples of a group's summand = the kernel's group sum.
proof fn lemma_tuples_sum(
    rows: Seq<LineitemRow>,
    e: int,
    cutoff: int,
    d: int,
    rf: Seq<u8>,
    ls: Seq<u8>,
    hf: spec_fn(LineitemRow) -> int,
    ff: spec_fn(Tuple) -> int,
)
    requires
        cutoff + e == 10561 - d,
        forall|t: Tuple| #[trigger] ff(t) == if t.lineitem.l_returnflag == rf && t.lineitem.l_linestatus == ls { hf(Lineitem_unshift(t.lineitem, e)) } else { 0 },
    ensures
        gsum(rows, cutoff, rf, ls, hf) == sum(tuples(db_at(rows, e), params_of(d)), ff),
{
    let gd = db_at(rows, e);
    let g = |lineitem: LineitemRow| tuples_1(gd, params_of(d), lineitem);
    lemma_sum_flat_map(gd.lineitem, g, ff);
    let gf = |lineitem: LineitemRow| sum(tuples_1(gd, params_of(d), lineitem), ff);
    lemma_sum_ext_all(gd.lineitem, |a: LineitemRow| sum(g(a), ff), gf);
    let hh = |l: LineitemRow| if in_group(l, cutoff, rf, ls) { hf(l) } else { 0 };
    assert forall|l: LineitemRow| #[trigger] hh(l) == gf(shift_row(l, e)) by {
        lemma_sum_opt(where_clause(gd, params_of(d), shift_row(l, e)), Tuple { lineitem: shift_row(l, e) }, ff);
        assert(Lineitem_unshift(shift_row(l, e), e) == l);
    }
    lemma_sum_map(rows, |l: LineitemRow| shift_row(l, e), gf, hh);
}

#[allow(non_snake_case)]
pub open spec fn Lineitem_unshift(l: LineitemRow, e: int) -> LineitemRow {
    LineitemRow { l_shipdate: l.l_shipdate - e, ..l }
}

} // verus!
