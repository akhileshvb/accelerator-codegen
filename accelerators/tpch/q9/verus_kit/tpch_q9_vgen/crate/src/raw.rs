// Scan rows as signed +1 events: how the general state is built from the scans.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use crate::bagsig::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_unit_events<R>(rows: Seq<R>, f: spec_fn(R) -> int)
    ensures sum(rows.map_values(|x: R| (x, 1int)), signed(f)) == sum(rows, f),
{
    lemma_sum_map(rows, |x: R| (x, 1int), signed(f), f);
}

/// A scan is the table's events with sign +1.
pub proof fn lemma_scan_built<R>(table: Seq<R>, rows: Seq<R>, events: Seq<(R, int)>)
    requires forall|f: spec_fn(R) -> int| #[trigger] sum(rows, f) == sum(table, f), events =~= rows.map_values(|x: R| (x, 1int)),
    ensures built_from(table, events),
{
    assert forall|f: spec_fn(R) -> int| #[trigger] sum(table, f) == sum(Seq::<R>::empty(), f) + sum(events, |d: (R, int)| d.1 * f(d.0)) by {
        assert(signed(f) =~= (|d: (R, int)| d.1 * f(d.0)));
        lemma_sum_empty(Seq::<R>::empty(), f);
        lemma_unit_events(rows, f);
        assert(sum(rows, f) == sum(table, f));
        assert(sum(events, signed(f)) == sum(rows.map_values(|x: R| (x, 1int)), signed(f)));
    }
}

pub fn copy_part_in(r: &PartIn) -> (c: PartIn)
    ensures part_in_view(c) == part_in_view(*r),
{
    PartIn { p_partkey: r.p_partkey, p_name: copy_bytes(&r.p_name) }
}

pub fn copy_nation_in(r: &NationIn) -> (c: NationIn)
    ensures nation_in_view(c) == nation_in_view(*r),
{
    NationIn { n_nationkey: r.n_nationkey, n_name: copy_bytes(&r.n_name) }
}

pub fn part_events(rows: &Vec<PartIn>) -> (r: Vec<PartDelta>)
    ensures part_delta_seq_view(r@) =~= part_in_seq_view(rows@).map_values(|x: PartRow| (x, 1int)),
{
    let mut r: Vec<PartDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> part_in_view(r@[j].row) == part_in_view(rows@[j]) && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(PartDelta { row: copy_part_in(&rows[i]), sign: 1 });
        i += 1;
    }
    r
}

pub fn nation_events(rows: &Vec<NationIn>) -> (r: Vec<NationDelta>)
    ensures nation_delta_seq_view(r@) =~= nation_in_seq_view(rows@).map_values(|x: NationRow| (x, 1int)),
{
    let mut r: Vec<NationDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> nation_in_view(r@[j].row) == nation_in_view(rows@[j]) && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(NationDelta { row: copy_nation_in(&rows[i]), sign: 1 });
        i += 1;
    }
    r
}

pub fn order_events(rows: &[OrderIn]) -> (r: Vec<OrderDelta>)
    ensures order_delta_seq_view(r@) =~= order_in_seq_view(rows@).map_values(|x: OrdersRow| (x, 1int)),
{
    let mut r: Vec<OrderDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| #![trigger r@[j]] 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        let ghost old_r = r@;
        r.push(OrderDelta { row: rows[i], sign: 1 });
        proof {
            assert forall|j: int| #![trigger r@[j]] 0 <= j < i + 1 implies r@[j].row == rows@[j] && r@[j].sign == 1 by {
                if j < i { assert(r@[j] == old_r[j]); }
            }
        }
        i += 1;
    }
    r
}

pub fn supplier_events(rows: &[SupplierIn]) -> (r: Vec<SupplierDelta>)
    ensures supplier_delta_seq_view(r@) =~= supplier_in_seq_view(rows@).map_values(|x: SupplierRow| (x, 1int)),
{
    let mut r: Vec<SupplierDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| #![trigger r@[j]] 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        let ghost old_r = r@;
        r.push(SupplierDelta { row: rows[i], sign: 1 });
        proof {
            assert forall|j: int| #![trigger r@[j]] 0 <= j < i + 1 implies r@[j].row == rows@[j] && r@[j].sign == 1 by {
                if j < i { assert(r@[j] == old_r[j]); }
            }
        }
        i += 1;
    }
    r
}

pub fn partsupp_events(rows: &[PartsuppIn]) -> (r: Vec<PartsuppDelta>)
    ensures partsupp_delta_seq_view(r@) =~= partsupp_in_seq_view(rows@).map_values(|x: PartsuppRow| (x, 1int)),
{
    let mut r: Vec<PartsuppDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| #![trigger r@[j]] 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        let ghost old_r = r@;
        r.push(PartsuppDelta { row: rows[i], sign: 1 });
        proof {
            assert forall|j: int| #![trigger r@[j]] 0 <= j < i + 1 implies r@[j].row == rows@[j] && r@[j].sign == 1 by {
                if j < i { assert(r@[j] == old_r[j]); }
            }
        }
        i += 1;
    }
    r
}

pub fn line_events(rows: &[LineIn]) -> (r: Vec<LineDelta>)
    ensures line_delta_seq_view(r@) =~= line_in_seq_view(rows@).map_values(|x: LineitemRow| (x, 1int)),
{
    let mut r: Vec<LineDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| #![trigger r@[j]] 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        let ghost old_r = r@;
        r.push(LineDelta { row: rows[i], sign: 1 });
        proof {
            assert forall|j: int| #![trigger r@[j]] 0 <= j < i + 1 implies r@[j].row == rows@[j] && r@[j].sign == 1 by {
                if j < i { assert(r@[j] == old_r[j]); }
            }
        }
        i += 1;
    }
    r
}

}
