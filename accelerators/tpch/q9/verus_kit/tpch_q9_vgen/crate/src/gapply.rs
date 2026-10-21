// The general state and its transitions: every index kept exact under every transaction.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::wide::*;
use crate::pmap::*;
use crate::gidx::*;
use crate::gnames::*;
use crate::gagg::*;
use crate::gstep::*;
use crate::ingest::{signed, dates_w, sup_w, ps_w};
use crate::applyspec::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct General {
    pub dates: PMap,
    pub sup: PMap,
    pub pairs: PMap,
    pub agg: PMap,
    pub pnames: NPages,
    pub nations: Arc<Vec<NCell>>,
}

pub open spec fn general_of(g: General, db: Db) -> bool {
    &&& dates_state(g.dates, db)
    &&& sup_state(g.sup, db)
    &&& pairs_state(g.pairs, db)
    &&& agg_state(g.agg, db)
    &&& pnames_state(g.pnames, db)
    &&& nations_state((*g.nations)@, db)
}

pub open spec fn db_empty() -> Db {
    Db { lineitem: Seq::empty(), nation: Seq::empty(), orders: Seq::empty(), part: Seq::empty(), partsupp: Seq::empty(), supplier: Seq::empty() }
}

pub fn general_empty() -> (g: General)
    ensures general_of(g, db_empty()),
{
    let d0 = pm_empty();
    let s0 = pm_empty();
    let p0 = pm_empty();
    let a0 = pm_empty();
    let n0 = pn_empty();
    let g = General { dates: d0, sup: s0, pairs: p0, agg: a0, pnames: n0, nations: Arc::new(Vec::new()) };
    proof {
        let db = db_empty();
        assert forall|k: i64, y: int| #[trigger] pm_at(g.dates, k, y, 0, 0) == dates(db, k as int, y) by {
            lemma_sum_empty(db.orders, |row: OrdersRow| if row.o_orderkey == k as int && civil_year(row.o_orderdate) == y { 1int } else { 0int });
            assert(d0.at(k).is_none());
            assert(pm_at(g.dates, k, y, 0, 0) == 0);
        }
        assert forall|k: i64, n: int| #[trigger] pm_at(g.sup, k, n, 0, 0) == supplier_count(db, k as int, n) by {
            lemma_sum_empty(db.supplier, |row: SupplierRow| if row.s_suppkey == k as int && row.s_nationkey == n { 1int } else { 0int });
            assert(s0.at(k).is_none());
            assert(pm_at(g.sup, k, n, 0, 0) == 0);
        }
        assert forall|k: i64, s: int| #[trigger] pm_at(g.pairs, k, s, 0, 0) == pair_count(db, k as int, s) && pm_at(g.pairs, k, s, 0, 1) == pair_cost(db, k as int, s) by {
            assert(p0.at(k).is_none());
            lemma_sum_empty(db.partsupp, |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { 1int } else { 0int });
            lemma_sum_empty(db.partsupp, |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
        }
        assert forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 implies #[trigger] pm_at(g.agg, k, s, y, f) == moments(db, k as int, s, y, f) by {
            assert(a0.at(k).is_none());
            lemma_sum_empty(db.lineitem, |row: LineitemRow| if row.l_partkey == k as int && row.l_suppkey == s { dates(db, row.l_orderkey, y) * line_weight(row, f) } else { 0int });
        }
        assert forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_at(g.pnames, k, f) == sum(db.part, part_w(k as int, f)) by {
            assert(n0.at(k).is_none());
            lemma_sum_empty(db.part, part_w(k as int, f));
        }
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum((*g.nations)@, f) == sum(db.nation, nation_wt(f)) by {
            lemma_sum_empty(db.nation, nation_wt(f));
            lemma_sum_empty((*g.nations)@, ncell_w(f));
        }
    }
    g
}

pub proof fn lemma_bag_w<R>(before: Seq<R>, after: Seq<R>, delta: Seq<(R, int)>, w: spec_fn(R) -> int)
    requires bag_step(before, after, delta),
    ensures sum(after, w) == sum(before, w) + sum(delta, signed(w)),
{
    assert(signed(w) =~= (|d: (R, int)| d.1 * w(d.0)));
}

#[verifier::spinoff_prover]
pub fn general_apply(g: &General, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta],
    order_keys: &[i64], order_rows: &[LineIn], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (r: General)
    requires general_of(*g, db),
        bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(lineitem_delta@)),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nation_delta@)),
        bag_step(db.orders, db2.orders, order_delta_seq_view(orders_delta@)),
        bag_step(db.part, db2.part, part_delta_seq_view(part_delta@)),
        bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(partsupp_delta@)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(supplier_delta@)),
        rescan_lines_for_orders_ok(db2, order_keys@, order_rows@),
        order_keys@ == okeys_of(orders_delta@),
    ensures general_of(r, db2),
{
    let mut dov: Ov = Work::new();
    ingest_orders(&g.dates, &mut dov, orders_delta);
    let mut sov: Ov = Work::new();
    ingest_suppliers(&g.sup, &mut sov, supplier_delta);
    let mut pov: Ov = Work::new();
    ingest_partsupp(&g.pairs, &mut pov, partsupp_delta);
    let mut nov: NOv = Work::new();
    ingest_parts(&g.pnames, &mut nov, part_delta);
    let nations = ingest_nations(&g.nations, nation_delta);
    let mut aov: Ov = Work::new();
    agg_lines(&g.dates, &g.agg, &mut aov, lineitem_delta);
    agg_rows(&g.dates, &dov, &g.agg, &mut aov, order_rows);
    let ghost dov_g = dov;
    let ghost aov_g = aov;
    let ghost sov_g = sov;
    let ghost pov_g = pov;
    let ghost nov_g = nov;
    proof {
        assert forall|k: i64, y: int| #[trigger] ov_at(g.dates, dov_g, k, y, 0, 0) == dates(db2, k as int, y) by {
            lemma_bag_w(db.orders, db2.orders, order_delta_seq_view(orders_delta@), dates_w(k as int, y));
            lemma_sum_ext_all(db.orders, dates_w(k as int, y), |row: OrdersRow| if row.o_orderkey == k as int && civil_year(row.o_orderdate) == y { 1int } else { 0int });
            lemma_sum_ext_all(db2.orders, dates_w(k as int, y), |row: OrdersRow| if row.o_orderkey == k as int && civil_year(row.o_orderdate) == y { 1int } else { 0int });
        }
        assert forall|k: i64, n: int| #[trigger] ov_at(g.sup, sov_g, k, n, 0, 0) == supplier_count(db2, k as int, n) by {
            lemma_bag_w(db.supplier, db2.supplier, supplier_delta_seq_view(supplier_delta@), sup_w(k as int, n));
            lemma_sum_ext_all(db.supplier, sup_w(k as int, n), |row: SupplierRow| if row.s_suppkey == k as int && row.s_nationkey == n { 1int } else { 0int });
            lemma_sum_ext_all(db2.supplier, sup_w(k as int, n), |row: SupplierRow| if row.s_suppkey == k as int && row.s_nationkey == n { 1int } else { 0int });
        }
        assert forall|k: i64, s: int| #[trigger] ov_at(g.pairs, pov_g, k, s, 0, 0) == pair_count(db2, k as int, s) && ov_at(g.pairs, pov_g, k, s, 0, 1) == pair_cost(db2, k as int, s) by {
            lemma_bag_w(db.partsupp, db2.partsupp, partsupp_delta_seq_view(partsupp_delta@), ps_w(k as int, s, 0));
            lemma_bag_w(db.partsupp, db2.partsupp, partsupp_delta_seq_view(partsupp_delta@), ps_w(k as int, s, 1));
            lemma_sum_ext_all(db.partsupp, ps_w(k as int, s, 0), |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { 1int } else { 0int });
            lemma_sum_ext_all(db2.partsupp, ps_w(k as int, s, 0), |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { 1int } else { 0int });
            lemma_sum_ext_all(db.partsupp, ps_w(k as int, s, 1), |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
            lemma_sum_ext_all(db2.partsupp, ps_w(k as int, s, 1), |row: PartsuppRow| if row.ps_partkey == k as int && row.ps_suppkey == s { row.ps_supplycost } else { 0int });
        }
        assert forall|k: i64, f: spec_fn((int, Seq<u8>)) -> int| #[trigger] pn_ov_at(g.pnames, nov_g, k, f) == sum(db2.part, part_w(k as int, f)) by {
            lemma_bag_w(db.part, db2.part, part_delta_seq_view(part_delta@), part_w(k as int, f));
        }
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] ncell_sum(nations@, f) == sum(db2.nation, nation_wt(f)) by {
            lemma_bag_w(db.nation, db2.nation, nation_delta_seq_view(nation_delta@), nation_wt(f));
        }
        assert forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(g.agg, aov_g, k, s, y, f) == moments(db2, k as int, s, y, f) by {
            lemma_moments_step(db, db2, lineitem_delta@, orders_delta@, order_rows@, order_keys@, k as int, s, y, f);
            lemma_sum_ext_all(lineitem_delta@, line_old_term(g.dates, k as int, s, y, f),
                |ld: LineDelta| if ld.row.l_partkey == k as int && ld.row.l_suppkey == s { ld.sign as int * dates(db, ld.row.l_orderkey as int, y) * line_weight(line_in_view(ld.row), f) } else { 0int });
            lemma_sum_ext_all(order_rows@, line_move_term(g.dates, dov_g, k as int, s, y, f),
                |r: LineIn| if r.l_partkey == k as int && r.l_suppkey == s { (dates(db2, r.l_orderkey as int, y) - dates(db, r.l_orderkey as int, y)) * line_weight(line_in_view(r), f) } else { 0int });
        }
    }
    let dates2 = publish(&g.dates, dov);
    let sup2 = publish(&g.sup, sov);
    let pairs2 = publish(&g.pairs, pov);
    let agg2 = publish(&g.agg, aov);
    let pnames2 = pn_publish(&g.pnames, nov);
    let r = General { dates: dates2, sup: sup2, pairs: pairs2, agg: agg2, pnames: pnames2, nations: Arc::new(nations) };
    r
}

}
