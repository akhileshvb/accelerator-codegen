// The dimension indexes of the general path, built exactly from the event logs.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::wide::*;
use crate::wmap::*;
use crate::log::*;
use crate::query_proofs::matched_weight;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

/// The signed weight of an event: its sign times the weight of its row.
pub open spec fn signed<R>(w: spec_fn(R) -> int) -> spec_fn((R, int)) -> int {
    |d: (R, int)| d.1 * w(d.0)
}

pub open spec fn dates_w(k: int, y: int) -> spec_fn(OrdersRow) -> int {
    |r: OrdersRow| if r.o_orderkey == k && civil_year(r.o_orderdate) == y { 1int } else { 0int }
}

/// `wm` counts, per (order, year), the events of `events` (weighted by sign).
pub open spec fn dates_index(m: WMap, events: Seq<OrderDelta>) -> bool {
    m.valid() && forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == sum(order_delta_seq_view(events), signed(dates_w(k as int, y)))
        && wm_at(m, k, y, 1) == 0
}

fn ingest_orders(m: &mut WMap, chunk: &[OrderDelta])
    requires old(m).valid(),
    ensures final(m).valid(),
        forall|k: i64, y: int| #[trigger] wm_at(*final(m), k, y, 0)
            == wm_at(*old(m), k, y, 0) + sum(order_delta_seq_view(chunk@), signed(dates_w(k as int, y))),
        forall|k: i64, y: int| wm_at(*final(m), k, y, 1) == wm_at(*old(m), k, y, 1),
{
    let ghost base = *m;
    let zero = Wide::zero();
    let mut i = 0;
    proof {
        assert forall|k: i64, y: int| #[trigger] wm_at(*m, k, y, 0) == wm_at(base, k, y, 0) + sum(order_delta_seq_view(chunk@).take(0), signed(dates_w(k as int, y))) by {
            lemma_sum_empty(order_delta_seq_view(chunk@).take(0), signed(dates_w(k as int, y)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), m.valid(), zero@ == 0, base == *old(m),
            forall|k: i64, y: int| #[trigger] wm_at(*m, k, y, 0) == wm_at(base, k, y, 0) + sum(order_delta_seq_view(chunk@).take(i as int), signed(dates_w(k as int, y))),
            forall|k: i64, y: int| wm_at(*m, k, y, 1) == wm_at(base, k, y, 1),
        decreases chunk.len() - i,
    {
        let d = chunk[i];
        let y = year(d.row.o_orderdate);
        let sgn = Wide::from_i128(d.sign as i128);
        let ghost before = *m;
        wm_add(m, d.row.o_orderkey, y, &sgn, &zero);
        proof {
            let view = order_delta_seq_view(chunk@);
            assert forall|k: i64, yy: int| #[trigger] wm_at(*m, k, yy, 0) == wm_at(base, k, yy, 0) + sum(view.take(i as int + 1), signed(dates_w(k as int, yy))) by {
                lemma_sum_take(view, i as int, signed(dates_w(k as int, yy)));
                assert(view[i as int] == (order_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(order_delta_seq_view(chunk@).take(chunk@.len() as int) =~= order_delta_seq_view(chunk@)); }
}


/// bag_step from the empty table: the table is exactly the signed events.
pub open spec fn built_from<R>(table: Seq<R>, events: Seq<(R, int)>) -> bool {
    bag_step(Seq::<R>::empty(), table, events)
}

pub proof fn lemma_built_sum<R>(table: Seq<R>, events: Seq<(R, int)>, w: spec_fn(R) -> int)
    requires built_from(table, events),
    ensures sum(table, w) == sum(events, signed(w)),
{
    lemma_sum_empty(Seq::<R>::empty(), w);
}

/// Built from the log of orders: the dates index of the table.
pub fn dates_from_log(log: &Log<OrderDelta>) -> (m: WMap)
    ensures dates_index(m, log.events()),
{
    let mut m: WMap = Work::new();
    let mut j = 0;
    proof {
        assert forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == sum(order_delta_seq_view(flat(log.parts().take(0))), signed(dates_w(k as int, y))) by {
            assert(flat(log.parts().take(0)) =~= Seq::<OrderDelta>::empty());
            assert(order_delta_seq_view(Seq::<OrderDelta>::empty()) =~= Seq::<(OrdersRow, int)>::empty());
            lemma_sum_empty(Seq::<(OrdersRow, int)>::empty(), signed(dates_w(k as int, y)));
        }
    }
    while j < log.chunks.len()
        invariant j <= log.chunks.len(), m.valid(),
            forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == sum(order_delta_seq_view(flat(log.parts().take(j as int))), signed(dates_w(k as int, y))),
            forall|k: i64, y: int| wm_at(m, k, y, 1) == 0,
        decreases log.chunks.len() - j,
    {
        let ghost before = m;
        ingest_orders(&mut m, log.chunks[j].as_slice());
        proof {
            let parts = log.parts();
            assert(parts.take(j as int + 1).drop_last() =~= parts.take(j as int));
            assert(flat(parts.take(j as int + 1)) == flat(parts.take(j as int)) + parts[j as int]);
            assert(parts[j as int] == (*log.chunks@[j as int])@);
            assert forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == sum(order_delta_seq_view(flat(parts.take(j as int + 1))), signed(dates_w(k as int, y))) by {
                let a = flat(parts.take(j as int));
                let b = parts[j as int];
                assert(order_delta_seq_view(a + b) =~= order_delta_seq_view(a) + order_delta_seq_view(b));
                lemma_sum_concat(order_delta_seq_view(a), order_delta_seq_view(b), signed(dates_w(k as int, y)));
            }
        }
        j += 1;
    }
    proof { assert(log.parts().take(j as int) =~= log.parts()); }
    m
}

/// The index equals `dates` of any table the log builds.
pub proof fn lemma_dates_index_is_dates(m: WMap, events: Seq<OrderDelta>, db: Db)
    requires dates_index(m, events), built_from(db.orders, order_delta_seq_view(events)),
    ensures forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == dates(db, k as int, y),
{
    assert forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == dates(db, k as int, y) by {
        lemma_built_sum(db.orders, order_delta_seq_view(events), dates_w(k as int, y));
        lemma_sum_ext_all(db.orders, dates_w(k as int, y), |row: OrdersRow| if row.o_orderkey == k as int && civil_year(row.o_orderdate) == y { 1int } else { 0int });
    }
}


// ── suppliers: (supplier, nation) -> rows ──

pub open spec fn sup_w(k: int, n: int) -> spec_fn(SupplierRow) -> int {
    |r: SupplierRow| if r.s_suppkey == k && r.s_nationkey == n { 1int } else { 0int }
}

pub open spec fn sup_index(m: WMap, events: Seq<SupplierDelta>) -> bool {
    m.valid() && forall|k: i64, n: int| #[trigger] wm_at(m, k, n, 0) == sum(supplier_delta_seq_view(events), signed(sup_w(k as int, n)))
        && wm_at(m, k, n, 1) == 0
}

fn ingest_suppliers(m: &mut WMap, chunk: &[SupplierDelta])
    requires old(m).valid(),
    ensures final(m).valid(),
        forall|k: i64, n: int| #[trigger] wm_at(*final(m), k, n, 0)
            == wm_at(*old(m), k, n, 0) + sum(supplier_delta_seq_view(chunk@), signed(sup_w(k as int, n))),
        forall|k: i64, n: int| wm_at(*final(m), k, n, 1) == wm_at(*old(m), k, n, 1),
{
    let ghost base = *m;
    let zero = Wide::zero();
    let mut i = 0;
    proof {
        assert forall|k: i64, n: int| #[trigger] wm_at(*m, k, n, 0) == wm_at(base, k, n, 0) + sum(supplier_delta_seq_view(chunk@).take(0), signed(sup_w(k as int, n))) by {
            lemma_sum_empty(supplier_delta_seq_view(chunk@).take(0), signed(sup_w(k as int, n)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), m.valid(), zero@ == 0, base == *old(m),
            forall|k: i64, n: int| #[trigger] wm_at(*m, k, n, 0) == wm_at(base, k, n, 0) + sum(supplier_delta_seq_view(chunk@).take(i as int), signed(sup_w(k as int, n))),
            forall|k: i64, n: int| wm_at(*m, k, n, 1) == wm_at(base, k, n, 1),
        decreases chunk.len() - i,
    {
        let d = chunk[i];
        let sgn = Wide::from_i128(d.sign as i128);
        wm_add(m, d.row.s_suppkey, d.row.s_nationkey, &sgn, &zero);
        proof {
            let view = supplier_delta_seq_view(chunk@);
            assert forall|k: i64, n: int| #[trigger] wm_at(*m, k, n, 0) == wm_at(base, k, n, 0) + sum(view.take(i as int + 1), signed(sup_w(k as int, n))) by {
                lemma_sum_take(view, i as int, signed(sup_w(k as int, n)));
                assert(view[i as int] == (supplier_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(supplier_delta_seq_view(chunk@).take(chunk@.len() as int) =~= supplier_delta_seq_view(chunk@)); }
}

pub fn sup_from_log(log: &Log<SupplierDelta>) -> (m: WMap)
    ensures sup_index(m, log.events()),
{
    let mut m: WMap = Work::new();
    let mut j = 0;
    proof {
        assert forall|k: i64, n: int| #[trigger] wm_at(m, k, n, 0) == sum(supplier_delta_seq_view(flat(log.parts().take(0))), signed(sup_w(k as int, n))) by {
            assert(flat(log.parts().take(0)) =~= Seq::<SupplierDelta>::empty());
            assert(supplier_delta_seq_view(Seq::<SupplierDelta>::empty()) =~= Seq::<(SupplierRow, int)>::empty());
            lemma_sum_empty(Seq::<(SupplierRow, int)>::empty(), signed(sup_w(k as int, n)));
        }
    }
    while j < log.chunks.len()
        invariant j <= log.chunks.len(), m.valid(),
            forall|k: i64, n: int| #[trigger] wm_at(m, k, n, 0) == sum(supplier_delta_seq_view(flat(log.parts().take(j as int))), signed(sup_w(k as int, n))),
            forall|k: i64, n: int| wm_at(m, k, n, 1) == 0,
        decreases log.chunks.len() - j,
    {
        ingest_suppliers(&mut m, log.chunks[j].as_slice());
        proof {
            let parts = log.parts();
            assert(parts.take(j as int + 1).drop_last() =~= parts.take(j as int));
            assert(flat(parts.take(j as int + 1)) == flat(parts.take(j as int)) + parts[j as int]);
            assert(parts[j as int] == (*log.chunks@[j as int])@);
            assert forall|k: i64, n: int| #[trigger] wm_at(m, k, n, 0) == sum(supplier_delta_seq_view(flat(parts.take(j as int + 1))), signed(sup_w(k as int, n))) by {
                let a = flat(parts.take(j as int));
                let b = parts[j as int];
                assert(supplier_delta_seq_view(a + b) =~= supplier_delta_seq_view(a) + supplier_delta_seq_view(b));
                lemma_sum_concat(supplier_delta_seq_view(a), supplier_delta_seq_view(b), signed(sup_w(k as int, n)));
            }
        }
        j += 1;
    }
    proof { assert(log.parts().take(j as int) =~= log.parts()); }
    m
}

pub proof fn lemma_sup_index_is_count(m: WMap, events: Seq<SupplierDelta>, db: Db)
    requires sup_index(m, events), built_from(db.supplier, supplier_delta_seq_view(events)),
    ensures forall|k: i64, n: int| #[trigger] wm_at(m, k, n, 0) == supplier_count(db, k as int, n),
{
    assert forall|k: i64, n: int| #[trigger] wm_at(m, k, n, 0) == supplier_count(db, k as int, n) by {
        lemma_built_sum(db.supplier, supplier_delta_seq_view(events), sup_w(k as int, n));
        lemma_sum_ext_all(db.supplier, sup_w(k as int, n), |row: SupplierRow| if row.s_suppkey == k as int && row.s_nationkey == n { 1int } else { 0int });
    }
}

// ── partsupp: (part, supplier) -> (rows, cost) ──

pub open spec fn ps_w(k: int, s: int, f: int) -> spec_fn(PartsuppRow) -> int {
    |r: PartsuppRow| if r.ps_partkey == k && r.ps_suppkey == s { if f == 0 { 1int } else { r.ps_supplycost } } else { 0int }
}

pub open spec fn ps_index(m: WMap, events: Seq<PartsuppDelta>) -> bool {
    m.valid() && forall|k: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(m, k, s, f) == sum(partsupp_delta_seq_view(events), signed(ps_w(k as int, s, f)))
}

fn ingest_partsupp(m: &mut WMap, chunk: &[PartsuppDelta])
    requires old(m).valid(),
    ensures final(m).valid(),
        forall|k: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(m), k, s, f)
            == wm_at(*old(m), k, s, f) + sum(partsupp_delta_seq_view(chunk@), signed(ps_w(k as int, s, f))),
{
    let ghost base = *m;
    let mut i = 0;
    proof {
        assert forall|k: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*m, k, s, f) == wm_at(base, k, s, f) + sum(partsupp_delta_seq_view(chunk@).take(0), signed(ps_w(k as int, s, f))) by {
            lemma_sum_empty(partsupp_delta_seq_view(chunk@).take(0), signed(ps_w(k as int, s, f)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), m.valid(), base == *old(m),
            forall|k: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*m, k, s, f) == wm_at(base, k, s, f) + sum(partsupp_delta_seq_view(chunk@).take(i as int), signed(ps_w(k as int, s, f))),
        decreases chunk.len() - i,
    {
        let d = chunk[i];
        let sgn = Wide::from_i128(d.sign as i128);
        let cost = sgn.mul(&Wide::from_i128(d.row.ps_supplycost as i128));
        wm_add(m, d.row.ps_partkey, d.row.ps_suppkey, &sgn, &cost);
        proof {
            let view = partsupp_delta_seq_view(chunk@);
            assert forall|k: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*m, k, s, f) == wm_at(base, k, s, f) + sum(view.take(i as int + 1), signed(ps_w(k as int, s, f))) by {
                lemma_sum_take(view, i as int, signed(ps_w(k as int, s, f)));
                assert(view[i as int] == (partsupp_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(partsupp_delta_seq_view(chunk@).take(chunk@.len() as int) =~= partsupp_delta_seq_view(chunk@)); }
}

pub fn ps_from_log(log: &Log<PartsuppDelta>) -> (m: WMap)
    ensures ps_index(m, log.events()),
{
    let mut m: WMap = Work::new();
    let mut j = 0;
    proof {
        assert forall|k: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(m, k, s, f) == sum(partsupp_delta_seq_view(flat(log.parts().take(0))), signed(ps_w(k as int, s, f))) by {
            assert(flat(log.parts().take(0)) =~= Seq::<PartsuppDelta>::empty());
            assert(partsupp_delta_seq_view(Seq::<PartsuppDelta>::empty()) =~= Seq::<(PartsuppRow, int)>::empty());
            lemma_sum_empty(Seq::<(PartsuppRow, int)>::empty(), signed(ps_w(k as int, s, f)));
        }
    }
    while j < log.chunks.len()
        invariant j <= log.chunks.len(), m.valid(),
            forall|k: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(m, k, s, f) == sum(partsupp_delta_seq_view(flat(log.parts().take(j as int))), signed(ps_w(k as int, s, f))),
        decreases log.chunks.len() - j,
    {
        ingest_partsupp(&mut m, log.chunks[j].as_slice());
        proof {
            let parts = log.parts();
            assert(parts.take(j as int + 1).drop_last() =~= parts.take(j as int));
            assert(flat(parts.take(j as int + 1)) == flat(parts.take(j as int)) + parts[j as int]);
            assert(parts[j as int] == (*log.chunks@[j as int])@);
            assert forall|k: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(m, k, s, f) == sum(partsupp_delta_seq_view(flat(parts.take(j as int + 1))), signed(ps_w(k as int, s, f))) by {
                let a = flat(parts.take(j as int));
                let b = parts[j as int];
                assert(partsupp_delta_seq_view(a + b) =~= partsupp_delta_seq_view(a) + partsupp_delta_seq_view(b));
                lemma_sum_concat(partsupp_delta_seq_view(a), partsupp_delta_seq_view(b), signed(ps_w(k as int, s, f)));
            }
        }
        j += 1;
    }
    proof { assert(log.parts().take(j as int) =~= log.parts()); }
    m
}

pub proof fn lemma_ps_index_is_pairs(m: WMap, events: Seq<PartsuppDelta>, db: Db)
    requires ps_index(m, events), built_from(db.partsupp, partsupp_delta_seq_view(events)),
    ensures forall|k: i64, s: int| #[trigger] wm_at(m, k, s, 0) == pair_count(db, k as int, s) && wm_at(m, k, s, 1) == pair_cost(db, k as int, s),
{
    assert forall|k: i64, s: int| #[trigger] wm_at(m, k, s, 0) == pair_count(db, k as int, s) && wm_at(m, k, s, 1) == pair_cost(db, k as int, s) by {
        assert(wm_at(m, k, s, 0) == sum(partsupp_delta_seq_view(events), signed(ps_w(k as int, s, 0))));
        assert(wm_at(m, k, s, 1) == sum(partsupp_delta_seq_view(events), signed(ps_w(k as int, s, 1))));
        lemma_built_sum(db.partsupp, partsupp_delta_seq_view(events), ps_w(k as int, s, 0));
        lemma_built_sum(db.partsupp, partsupp_delta_seq_view(events), ps_w(k as int, s, 1));
        lemma_sum_ext_all(db.partsupp, ps_w(k as int, s, 0), |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { 1int } else { 0int });
        lemma_sum_ext_all(db.partsupp, ps_w(k as int, s, 1), |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
    }
}


// ── parts: part -> rows whose name matches the pattern ──

pub open spec fn gr_index(m: WMap, events: Seq<PartDelta>, p: Seq<u8>) -> bool {
    m.valid() && forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0)
            == if y == 0 { sum(part_delta_seq_view(events), signed(matched_weight(p, k as int))) } else { 0int }
        && wm_at(m, k, y, 1) == 0
}

fn ingest_parts(m: &mut WMap, chunk: &[PartDelta], p: &[u8])
    requires old(m).valid(),
    ensures final(m).valid(),
        forall|k: i64, y: int| #[trigger] wm_at(*final(m), k, y, 0)
            == wm_at(*old(m), k, y, 0) + if y == 0 { sum(part_delta_seq_view(chunk@), signed(matched_weight(p@, k as int))) } else { 0int },
        forall|k: i64, y: int| wm_at(*final(m), k, y, 1) == wm_at(*old(m), k, y, 1),
{
    let ghost base = *m;
    let zero = Wide::zero();
    let mut i = 0;
    proof {
        assert forall|k: i64, y: int| #[trigger] wm_at(*m, k, y, 0) == wm_at(base, k, y, 0) + if y == 0 { sum(part_delta_seq_view(chunk@).take(0), signed(matched_weight(p@, k as int))) } else { 0int } by {
            lemma_sum_empty(part_delta_seq_view(chunk@).take(0), signed(matched_weight(p@, k as int)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), m.valid(), zero@ == 0, base == *old(m),
            forall|k: i64, y: int| #[trigger] wm_at(*m, k, y, 0) == wm_at(base, k, y, 0) + if y == 0 { sum(part_delta_seq_view(chunk@).take(i as int), signed(matched_weight(p@, k as int))) } else { 0int },
            forall|k: i64, y: int| wm_at(*m, k, y, 1) == wm_at(base, k, y, 1),
        decreases chunk.len() - i,
    {
        let d = &chunk[i];
        let hit = verus_prooflib::like_match_exec(d.row.p_name.as_slice(), p);
        let ghost before = *m;
        if hit {
            let sgn = Wide::from_i128(d.sign as i128);
            wm_add(m, d.row.p_partkey, 0, &sgn, &zero);
        }
        proof {
            let view = part_delta_seq_view(chunk@);
            assert forall|k: i64, y: int| #[trigger] wm_at(*m, k, y, 0) == wm_at(base, k, y, 0) + if y == 0 { sum(view.take(i as int + 1), signed(matched_weight(p@, k as int))) } else { 0int } by {
                lemma_sum_take(view, i as int, signed(matched_weight(p@, k as int)));
                assert(view[i as int] == (part_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(part_delta_seq_view(chunk@).take(chunk@.len() as int) =~= part_delta_seq_view(chunk@)); }
}

pub fn gr_from_log(log: &Log<PartDelta>, p: &[u8]) -> (m: WMap)
    ensures gr_index(m, log.events(), p@),
{
    let mut m: WMap = Work::new();
    let mut j = 0;
    proof {
        assert forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == if y == 0 { sum(part_delta_seq_view(flat(log.parts().take(0))), signed(matched_weight(p@, k as int))) } else { 0int } by {
            assert(flat(log.parts().take(0)) =~= Seq::<PartDelta>::empty());
            assert(part_delta_seq_view(Seq::<PartDelta>::empty()) =~= Seq::<(PartRow, int)>::empty());
            lemma_sum_empty(Seq::<(PartRow, int)>::empty(), signed(matched_weight(p@, k as int)));
        }
    }
    while j < log.chunks.len()
        invariant j <= log.chunks.len(), m.valid(),
            forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == if y == 0 { sum(part_delta_seq_view(flat(log.parts().take(j as int))), signed(matched_weight(p@, k as int))) } else { 0int },
            forall|k: i64, y: int| wm_at(m, k, y, 1) == 0,
        decreases log.chunks.len() - j,
    {
        ingest_parts(&mut m, log.chunks[j].as_slice(), p);
        proof {
            let parts = log.parts();
            assert(parts.take(j as int + 1).drop_last() =~= parts.take(j as int));
            assert(flat(parts.take(j as int + 1)) == flat(parts.take(j as int)) + parts[j as int]);
            assert(parts[j as int] == (*log.chunks@[j as int])@);
            assert forall|k: i64, y: int| #[trigger] wm_at(m, k, y, 0) == if y == 0 { sum(part_delta_seq_view(flat(parts.take(j as int + 1))), signed(matched_weight(p@, k as int))) } else { 0int } by {
                let a = flat(parts.take(j as int));
                let b = parts[j as int];
                assert(part_delta_seq_view(a + b) =~= part_delta_seq_view(a) + part_delta_seq_view(b));
                lemma_sum_concat(part_delta_seq_view(a), part_delta_seq_view(b), signed(matched_weight(p@, k as int)));
            }
        }
        j += 1;
    }
    proof { assert(log.parts().take(j as int) =~= log.parts()); }
    m
}

pub proof fn lemma_gr_index_is_green(m: WMap, events: Seq<PartDelta>, p: Seq<u8>, db: Db)
    requires gr_index(m, events, p), built_from(db.part, part_delta_seq_view(events)),
    ensures forall|k: i64| #[trigger] wm_at(m, k, 0, 0) == green(filter_db(db, p), k as int),
{
    assert forall|k: i64| #[trigger] wm_at(m, k, 0, 0) == green(filter_db(db, p), k as int) by {
        lemma_built_sum(db.part, part_delta_seq_view(events), matched_weight(p, k as int));
        crate::query_proofs::lemma_matched_green(db, p, k as int);
    }
}

}
