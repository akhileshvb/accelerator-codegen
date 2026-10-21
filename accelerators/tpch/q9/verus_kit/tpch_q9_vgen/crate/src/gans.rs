// The general answer: nation-year totals, regrouped by nation name, sorted.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::wide::*;
use crate::wmap::*;
use crate::pmap::*;
use crate::gnames::*;
use crate::gapply::*;
use crate::gread::*;
use crate::gproof::*;
use crate::groups::*;
use crate::emitw::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn tcell(t: WMap, n: Seq<u8>, y: int, f: int) -> spec_fn((int, Seq<u8>)) -> int {
    |kn: (int, Seq<u8>)| if kn.1 == n { wm_at(t, kn.0 as i64, y, f) } else { 0int }
}

/// g += every nation row's count times the nation's totals.
pub fn groups_from_cells(nations: &Vec<NCell>, t: &WMap) -> (g: Vec<GEnt>)
    ensures
        forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f) == ncell_sum(nations@, tcell(*t, n, y, f)),
        g_keys(g@).no_duplicates(),
{
    let mut g: Vec<GEnt> = Vec::new();
    let empty: Vec<Ent> = Vec::new();
    let mut i = 0;
    proof {
        assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f) == ncell_sum(nations@.take(0), tcell(*t, n, y, f)) by {
            lemma_sum_empty(g@, g_weight(n, y, f));
            lemma_sum_empty(nations@.take(0), ncell_w(tcell(*t, n, y, f)));
        }
        assert(g_keys(g@) =~= Seq::<(Seq<u8>, int)>::empty());
    }
    while i < nations.len()
        invariant i <= nations.len(), empty@.len() == 0,
            forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f) == ncell_sum(nations@.take(i as int), tcell(*t, n, y, f)),
            g_keys(g@).no_duplicates(),
        decreases nations.len() - i,
    {
        let e = &nations[i];
        let rows = bucket(t, e.k, &empty);
        let ghost before = g@;
        let mut j = 0;
        proof {
            assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f)
                == g_at(before, n, y, f) + if n == e.name@ { e.count@ * ent_at(rows@.take(0), y, f) } else { 0 } by {
                lemma_sum_empty(rows@.take(0), ent_weight(y, f));
                assert(e.count@ * 0int == 0) by (nonlinear_arith);
            }
        }
        while j < rows.len()
            invariant j <= rows.len(), i < nations.len(), e == &nations[i as int],
                forall|y: int, f: int| 0 <= f <= 1 ==> #[trigger] ent_at(rows@, y, f) == wm_at(*t, e.k, y, f),
                forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f)
                    == g_at(before, n, y, f) + if n == e.name@ { e.count@ * ent_at(rows@.take(j as int), y, f) } else { 0 },
                g_keys(before).no_duplicates() ==> g_keys(g@).no_duplicates(),
                g_keys(before).no_duplicates(),
            decreases rows.len() - j,
        {
            let c = &rows[j];
            let da = e.count.mul(&c.a);
            let db = e.count.mul(&c.b);
            bump_g(&mut g, &e.name, c.k2, &da, &db);
            proof {
                assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f)
                    == g_at(before, n, y, f) + if n == e.name@ { e.count@ * ent_at(rows@.take(j as int + 1), y, f) } else { 0 } by {
                    lemma_sum_take(rows@, j as int, ent_weight(y, f));
                    let prev = ent_at(rows@.take(j as int), y, f);
                    let ev = if c.k2 == y { if f == 0 { c.a@ } else { c.b@ } } else { 0int };
                    assert(ent_weight(y, f)(rows@[j as int]) == ev);
                    if n == e.name@ {
                        let cc = e.count@;
                        if y == c.k2 as int {
                            assert(cc * (prev + ev) == cc * prev + cc * ev) by (nonlinear_arith);
                        } else {
                            assert(cc * (prev + 0) == cc * prev) by (nonlinear_arith);
                        }
                    }
                }
            }
            j += 1;
        }
        proof {
            assert(rows@.take(rows.len() as int) =~= rows@);
            assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f) == ncell_sum(nations@.take(i as int + 1), tcell(*t, n, y, f)) by {
                lemma_sum_take(nations@, i as int, ncell_w(tcell(*t, n, y, f)));
                assert(ncell_w(tcell(*t, n, y, f))(nations@[i as int]) == if n == e.name@ { e.count@ * wm_at(*t, e.k, y, f) } else { 0int });
                assert((e.k as int) as i64 == e.k);
            }
        }
        i += 1;
    }
    proof { assert(nations@.take(nations.len() as int) =~= nations@); }
    g
}

proof fn lemma_ncell_fn_ext(rows: Seq<NCell>, f1: spec_fn((int, Seq<u8>)) -> int, f2: spec_fn((int, Seq<u8>)) -> int)
    requires forall|j: int| 0 <= j < rows.len() ==> f1(((#[trigger] rows[j]).k as int, rows[j].name@)) == f2((rows[j].k as int, rows[j].name@)),
    ensures ncell_sum(rows, f1) == ncell_sum(rows, f2),
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_ncell_fn_ext(rows.drop_last(), f1, f2);
    }
}


/// The answer for any pattern from the general state; None only when a profit does not fit.
pub fn general_answer(g: &General, p1: &[u8], Ghost(db): Ghost<Db>) -> (r: Option<Vec<(Vec<u8>, i64, i128)>>)
    requires general_of(*g, db),
    ensures
        r matches Some(out) ==> query_result(db, params(p1@), out_view(out@)),
        r is None ==> !answer_fits(db, params(p1@)),
{
    proof { assert(read_wf(*g)); }
    let t = read_t(g, p1);
    let groups = groups_from_cells(&g.nations, &t);
    proof {
        let pp = params(p1@);
        let dbp = filter_db(db, p1@);
        let keys = choose|keys: Seq<i64>| keys.no_duplicates() && (forall|k: i64| #[trigger] g.pnames.at(k).is_some() <==> keys.contains(k))
            && forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == sum(keys, |q: i64| contrib(*g, p1@, q, nk as int, y, f));
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(t, nk, y, f) == nation_total(dbp, nk as int, y, f) by {
            lemma_read_is_nation_total(*g, db, p1@, keys, nk as int, y, f);
        }
        assert forall|n: Seq<u8>, y: int| #[trigger] g_at(groups@, n, y, 0) == group_count(db, pp, n, y)
            && g_at(groups@, n, y, 1) == group_sum(db, pp, n, y, |row: Tuple| row.profit.amount) by {
            assert forall|f: int| 0 <= f <= 1 implies g_at(groups@, n, y, f) == group_database(dbp, n, y, f) by {
                let ft = tcell(t, n, y, f);
                let fd = |kn: (int, Seq<u8>)| if kn.1 == n { nation_total(dbp, kn.0, y, f) } else { 0int };
                assert forall|j: int| 0 <= j < g.nations@.len() implies ft(((#[trigger] g.nations@[j]).k as int, g.nations@[j].name@)) == fd((g.nations@[j].k as int, g.nations@[j].name@)) by {
                    let e = g.nations@[j];
                    assert((e.k as int) as i64 == e.k);
                }
                lemma_ncell_fn_ext(g.nations@, ft, fd);
                assert(nations_state((*g.nations)@, db));
                assert(ncell_sum(g.nations@, fd) == sum(db.nation, nation_wt(fd)));
                lemma_sum_ext_all(db.nation, nation_wt(fd), |row: NationRow| if row.n_name == n { nation_total(dbp, row.n_nationkey, y, f) } else { 0int });
                crate::meaning::lemma_filter_same(db, p1@);
                assert(dbp.nation == db.nation);
            }
            crate::query_proofs::lemma_query_groups(db, pp, n, y, 0);
            crate::query_proofs::lemma_query_groups(db, pp, n, y, 1);
        }
    }
    emit_groups(&groups, Ghost(db), Ghost(params(p1@)))
}

}
