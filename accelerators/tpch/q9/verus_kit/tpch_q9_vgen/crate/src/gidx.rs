// The general path's dimension indexes (order -> years, supplier -> nations, part -> suppliers'
// cost) and their incremental update from a transaction's deltas, through overlays.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use verus_prooflib::wide::Wide;
use crate::pmap::*;
use crate::bagsig::{signed, dates_w, sup_w, ps_w};

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn dates_state(m: PMap, db: Db) -> bool {
    m.valid() && pm_unique(m) && forall|k: i64, y: int| #[trigger] pm_at(m, k, y, 0, 0) == dates(db, k as int, y)
}

pub open spec fn sup_state(m: PMap, db: Db) -> bool {
    m.valid() && pm_unique(m) && forall|k: i64, n: int| #[trigger] pm_at(m, k, n, 0, 0) == supplier_count(db, k as int, n)
}

pub open spec fn pairs_state(m: PMap, db: Db) -> bool {
    m.valid() && pm_unique(m) && forall|k: i64, s: int| #[trigger] pm_at(m, k, s, 0, 0) == pair_count(db, k as int, s)
        && pm_at(m, k, s, 0, 1) == pair_cost(db, k as int, s)
}

pub fn ingest_orders(base: &PMap, ov: &mut Ov, chunk: &[OrderDelta])
    requires base.valid(), old(ov).valid(), pm_unique(*base), ov_unique(*base, *old(ov)),
    ensures final(ov).valid(), ov_unique(*base, *final(ov)),
        forall|k: i64, y: int| #[trigger] ov_at(*base, *final(ov), k, y, 0, 0)
            == ov_at(*base, *old(ov), k, y, 0, 0) + sum(order_delta_seq_view(chunk@), signed(dates_w(k as int, y))),
{
    let ghost start = *ov;
    let zero = Wide::zero();
    let mut i = 0;
    proof {
        assert forall|k: i64, y: int| #[trigger] ov_at(*base, *ov, k, y, 0, 0) == ov_at(*base, start, k, y, 0, 0) + sum(order_delta_seq_view(chunk@).take(0), signed(dates_w(k as int, y))) by {
            lemma_sum_empty(order_delta_seq_view(chunk@).take(0), signed(dates_w(k as int, y)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), base.valid(), ov.valid(), pm_unique(*base), ov_unique(*base, *ov), zero@ == 0, start == *old(ov),
            forall|k: i64, y: int| #[trigger] ov_at(*base, *ov, k, y, 0, 0) == ov_at(*base, start, k, y, 0, 0) + sum(order_delta_seq_view(chunk@).take(i as int), signed(dates_w(k as int, y))),
        decreases chunk.len() - i,
    {
        let d = chunk[i];
        let y = year(d.row.o_orderdate);
        let sgn = Wide::from_i128(d.sign as i128);
        ov_add(base, ov, d.row.o_orderkey, y, 0, &sgn, &zero, &zero);
        proof {
            let view = order_delta_seq_view(chunk@);
            assert forall|k: i64, yy: int| #[trigger] ov_at(*base, *ov, k, yy, 0, 0) == ov_at(*base, start, k, yy, 0, 0) + sum(view.take(i as int + 1), signed(dates_w(k as int, yy))) by {
                lemma_sum_take(view, i as int, signed(dates_w(k as int, yy)));
                assert(view[i as int] == (order_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(order_delta_seq_view(chunk@).take(chunk@.len() as int) =~= order_delta_seq_view(chunk@)); }
}

pub fn ingest_suppliers(base: &PMap, ov: &mut Ov, chunk: &[SupplierDelta])
    requires base.valid(), old(ov).valid(), pm_unique(*base), ov_unique(*base, *old(ov)),
    ensures final(ov).valid(), ov_unique(*base, *final(ov)),
        forall|k: i64, n: int| #[trigger] ov_at(*base, *final(ov), k, n, 0, 0)
            == ov_at(*base, *old(ov), k, n, 0, 0) + sum(supplier_delta_seq_view(chunk@), signed(sup_w(k as int, n))),
{
    let ghost start = *ov;
    let zero = Wide::zero();
    let mut i = 0;
    proof {
        assert forall|k: i64, n: int| #[trigger] ov_at(*base, *ov, k, n, 0, 0) == ov_at(*base, start, k, n, 0, 0) + sum(supplier_delta_seq_view(chunk@).take(0), signed(sup_w(k as int, n))) by {
            lemma_sum_empty(supplier_delta_seq_view(chunk@).take(0), signed(sup_w(k as int, n)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), base.valid(), ov.valid(), pm_unique(*base), ov_unique(*base, *ov), zero@ == 0, start == *old(ov),
            forall|k: i64, n: int| #[trigger] ov_at(*base, *ov, k, n, 0, 0) == ov_at(*base, start, k, n, 0, 0) + sum(supplier_delta_seq_view(chunk@).take(i as int), signed(sup_w(k as int, n))),
        decreases chunk.len() - i,
    {
        let d = chunk[i];
        let sgn = Wide::from_i128(d.sign as i128);
        ov_add(base, ov, d.row.s_suppkey, d.row.s_nationkey, 0, &sgn, &zero, &zero);
        proof {
            let view = supplier_delta_seq_view(chunk@);
            assert forall|k: i64, n: int| #[trigger] ov_at(*base, *ov, k, n, 0, 0) == ov_at(*base, start, k, n, 0, 0) + sum(view.take(i as int + 1), signed(sup_w(k as int, n))) by {
                lemma_sum_take(view, i as int, signed(sup_w(k as int, n)));
                assert(view[i as int] == (supplier_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(supplier_delta_seq_view(chunk@).take(chunk@.len() as int) =~= supplier_delta_seq_view(chunk@)); }
}

pub fn ingest_partsupp(base: &PMap, ov: &mut Ov, chunk: &[PartsuppDelta])
    requires base.valid(), old(ov).valid(), pm_unique(*base), ov_unique(*base, *old(ov)),
    ensures final(ov).valid(), ov_unique(*base, *final(ov)),
        forall|k: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] ov_at(*base, *final(ov), k, s, 0, f)
            == ov_at(*base, *old(ov), k, s, 0, f) + sum(partsupp_delta_seq_view(chunk@), signed(ps_w(k as int, s, f))),
{
    let ghost start = *ov;
    let zero = Wide::zero();
    let mut i = 0;
    proof {
        assert forall|k: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] ov_at(*base, *ov, k, s, 0, f) == ov_at(*base, start, k, s, 0, f) + sum(partsupp_delta_seq_view(chunk@).take(0), signed(ps_w(k as int, s, f))) by {
            lemma_sum_empty(partsupp_delta_seq_view(chunk@).take(0), signed(ps_w(k as int, s, f)));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), base.valid(), ov.valid(), pm_unique(*base), ov_unique(*base, *ov), zero@ == 0, start == *old(ov),
            forall|k: i64, s: int, f: int| 0 <= f <= 1 ==> #[trigger] ov_at(*base, *ov, k, s, 0, f) == ov_at(*base, start, k, s, 0, f) + sum(partsupp_delta_seq_view(chunk@).take(i as int), signed(ps_w(k as int, s, f))),
        decreases chunk.len() - i,
    {
        let d = chunk[i];
        let sgn = Wide::from_i128(d.sign as i128);
        let cost = sgn.mul(&Wide::from_i128(d.row.ps_supplycost as i128));
        ov_add(base, ov, d.row.ps_partkey, d.row.ps_suppkey, 0, &sgn, &cost, &zero);
        proof {
            let view = partsupp_delta_seq_view(chunk@);
            assert forall|k: i64, s: int, f: int| 0 <= f <= 1 implies #[trigger] ov_at(*base, *ov, k, s, 0, f) == ov_at(*base, start, k, s, 0, f) + sum(view.take(i as int + 1), signed(ps_w(k as int, s, f))) by {
                lemma_sum_take(view, i as int, signed(ps_w(k as int, s, f)));
                assert(view[i as int] == (partsupp_in_view(d.row), d.sign as int));
            }
        }
        i += 1;
    }
    proof { assert(partsupp_delta_seq_view(chunk@).take(chunk@.len() as int) =~= partsupp_delta_seq_view(chunk@)); }
}

}
