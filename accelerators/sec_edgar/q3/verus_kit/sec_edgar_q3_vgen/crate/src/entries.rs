// The rounded total of every group of a set of join records.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::float::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::ordered::lemma_ordered_unique;
use verus_prooflib::wide::Wide;
use crate::f64bits::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;
use crate::numeric::*;
use crate::rows::*;

verus! {

pub struct GEntry {
    pub name: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub cv: Wide,
    pub bits: Option<u64>,
    pub count: usize,
    pub sum: Wide,
    pub abs: Wide,
}

/// cv is the double `bits` that nearest-rounds x, or x itself when x is no double.
pub open spec fn rounded_as(x: int, cv: int, bits: Option<u64>) -> bool {
    match bits {
        Some(b) => f64_rounded(b, x) && f64_finite(b) && cv == f64_val(b),
        None => !f64_in_range(x) && cv == x,
    }
}

pub open spec fn gentry_ok(l: Seq<Rec>, e: GEntry) -> bool {
    &&& e.count as int == gsum(l, opt_bytes(e.name), opt_int(e.cik), one())
    &&& e.count >= 1
    &&& e.sum@ == gsum(l, opt_bytes(e.name), opt_int(e.cik), value())
    &&& e.abs@ == gsum(l, opt_bytes(e.name), opt_int(e.cik), magnitude())
    &&& rounded_as(e.sum@, e.cv@, e.bits)
    &&& float_within(e.cv@, e.sum@, e.abs@, e.count as int - 1)
}

pub open spec fn gentries_ok(l: Seq<Rec>, gs: Seq<GEntry>) -> bool {
    &&& forall|i: int| 0 <= i < gs.len() ==> gentry_ok(l, #[trigger] gs[i])
    &&& forall|i: int, j: int| 0 <= i < j < gs.len() ==>
        !(opt_bytes((#[trigger] gs[i]).name) == opt_bytes((#[trigger] gs[j]).name) && gs[i].cik == gs[j].cik)
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| gsum(l, gn, gc, one()) > 0 ==>
        exists|i: int| 0 <= i < gs.len() && opt_bytes((#[trigger] gs[i]).name) == gn && opt_int(gs[i].cik) == gc
}

pub proof fn lemma_abs_gsum(l: Seq<Rec>, gn: Option<Seq<u8>>, gc: Option<int>)
    ensures abs(gsum(l, gn, gc, value())) <= gsum(l, gn, gc, magnitude()),
    decreases l.len(),
{
    if l.len() > 0 {
        let rest = l.drop_last();
        let last = l.last();
        assert(l =~= rest.push(last));
        lemma_abs_gsum(rest, gn, gc);
        lemma_gsum_push(rest, last, gn, gc, value());
        lemma_gsum_push(rest, last, gn, gc, magnitude());
    } else {
        assert(l =~= Seq::<Rec>::empty());
    }
}

/// The cell's total rounded: exactly when the cell holds one record.
fn round_cell(c: &Cell, consts: &Consts) -> (r: (Wide, Option<u64>))
    requires
        consts_ok(*consts),
        c.count >= 1,
        f64_finite(c.v1),
        c.count == 1 ==> c.sum@ == f64_val(c.v1),
    ensures
        rounded_as(c.sum@, r.0@, r.1),
        c.count == 1 ==> r.0@ == c.sum@,
{
    if c.count == 1 {
        proof {
            lemma_f64_in_range(c.v1);
            assert(abs(f64_val(c.v1) - c.sum@) == 0);
        }
        (c.sum.dup(), Some(c.v1))
    } else {
        round_value(&c.sum, consts)
    }
}

/// The entries of every group of the cells.
pub fn entries_of(cells: &Index<Cell>, consts: &Consts, Ghost(l): Ghost<Seq<Rec>>) -> (es: Vec<GEntry>)
    requires
        groups_ok(*cells, l),
        consts_ok(*consts),
    ensures
        gentries_ok(l, es@),
{
    let rows = cells.rows();
    let ghost entries = cells.entries();
    proof {
        cells.model();
        lemma_ordered_unique(entries);
    }
    let mut es: Vec<GEntry> = Vec::new();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            groups_ok(*cells, l),
            consts_ok(*consts),
            rows@.map_values(|row: (Key, Arc<Cell>)| (row.0.view(), *row.1)) == entries,
            forall|a: int, b: int| 0 <= a < b < entries.len() ==> entries[a].0 != entries[b].0,
            forall|position: int| #![trigger entries[position]] 0 <= position < entries.len() ==> cells.at(entries[position].0) == Some(entries[position].1),
            es.len() == i,
            forall|j: int| 0 <= j < es.len() ==> gentry_ok(l, #[trigger] es[j]),
            forall|j: int| 0 <= j < es.len() ==> opt_bytes(es[j].name) == opt_bytes(rows@[j].1.name) && es[j].cik == rows@[j].1.cik,
        decreases rows.len() - i,
    {
        let cell = &*rows[i].1;
        proof {
            assert(entries[i as int] == (rows@[i as int].0.view(), *rows@[i as int].1));
            assert(cells.at(entries[i as int].0) == Some(entries[i as int].1));
            let (gn, gc) = (opt_bytes(cell.name), opt_int(cell.cik));
            assert(entries[i as int].0 == gk(gn, gc));
            assert(cells.at(gk(gn, gc)) is Some);
            assert(cell_ok(*cell, l, gn, gc));
            lemma_abs_gsum(l, gn, gc);
        }
        let (cv, bits) = round_cell(cell, consts);
        proof {
            let (gn, gc) = (opt_bytes(cell.name), opt_int(cell.cik));
            let (e, a, m) = (cell.sum@, cell.abs@, cell.count as int - 1);
            match bits {
                Some(b) => { if cell.count >= 2 { lemma_rounded_within(b, e, a, m); } },
                None => {},
            }
            assert(float_within(cv@, e, a, m));
        }
        let e = GEntry {
            name: match &cell.name { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
            cik: cell.cik, cv, bits, count: cell.count, sum: cell.sum.dup(), abs: cell.abs.dup(),
        };
        es.push(e);
        i += 1;
    }
    proof {
        assert forall|a: int, b: int| 0 <= a < b < es.len() implies
            !(opt_bytes((#[trigger] es[a]).name) == opt_bytes((#[trigger] es[b]).name) && es[a].cik == es[b].cik) by {
            assert(entries[a].0 != entries[b].0);
            assert(entries[a].0 == gk(opt_bytes(rows@[a].1.name), opt_int(rows@[a].1.cik)));
            assert(entries[b].0 == gk(opt_bytes(rows@[b].1.name), opt_int(rows@[b].1.cik)));
        }
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| gsum(l, gn, gc, one()) > 0 implies
            exists|i: int| 0 <= i < es.len() && opt_bytes((#[trigger] es[i]).name) == gn && opt_int(es[i].cik) == gc by {
            if cells.at(gk(gn, gc)) is None { assert(gsum(l, gn, gc, one()) == 0); }
            let m = choose|m: int| 0 <= m < entries.len() && entries[m].0 == gk(gn, gc);
            assert(entries[m] == (rows@[m].0.view(), *rows@[m].1));
            assert(cells.at(entries[m].0) == Some(entries[m].1));
            let cell = *rows@[m].1;
            assert(cell_ok(cell, l, gn, gc));
            assert(opt_bytes(es[m].name) == gn && opt_int(es[m].cik) == gc);
        }
    }
    es
}

}
