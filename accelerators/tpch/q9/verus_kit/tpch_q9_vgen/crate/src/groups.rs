// The general path's groups: per (nation name, year) the exact tuple count and profit,
// accumulated from the nation events and the nation-year totals.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::wide::*;
use crate::wmap::*;
use crate::log::*;
use crate::ingest::*;
use crate::linepass::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct GEnt { pub name: Vec<u8>, pub year: i64, pub a: Wide, pub b: Wide }

pub open spec fn g_weight(name: Seq<u8>, y: int, f: int) -> spec_fn(GEnt) -> int {
    |e: GEnt| if e.name@ == name && e.year == y { if f == 0 { e.a@ } else { e.b@ } } else { 0int }
}

pub open spec fn g_at(rows: Seq<GEnt>, name: Seq<u8>, y: int, f: int) -> int {
    sum(rows, g_weight(name, y, f))
}

pub open spec fn g_keys(rows: Seq<GEnt>) -> Seq<(Seq<u8>, int)> {
    rows.map_values(|e: GEnt| (e.name@, e.year as int))
}

/// rows[(name, year)] += (da, db).
pub fn bump_g(rows: &mut Vec<GEnt>, name: &Vec<u8>, year: i64, da: &Wide, db: &Wide)
    ensures
        forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(final(rows)@, n, y, f)
            == g_at(old(rows)@, n, y, f) + if n == name@ && y == year as int { delta_of(da@, db@, f) } else { 0 },
        g_keys(old(rows)@).no_duplicates() ==> g_keys(final(rows)@).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> !((#[trigger] rows@[position]).name@ == name@ && rows@[position].year == year),
        decreases rows.len() - index,
    {
        if rows[index].year == year && bytes_equal(&rows[index].name, name) {
            let a2 = rows[index].a.add(da);
            let b2 = rows[index].b.add(db);
            let entry = GEnt { name: copy_bytes(name), year, a: a2, b: b2 };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(rows@, n, y, f)
                    == g_at(before, n, y, f) + if n == name@ && y == year as int { delta_of(da@, db@, f) } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, g_weight(n, y, f));
                }
                assert(g_keys(rows@) =~= g_keys(before));
            }
            return;
        }
        index += 1;
    }
    let entry = GEnt { name: copy_bytes(name), year, a: da.dup(), b: db.dup() };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(rows@, n, y, f)
            == g_at(before, n, y, f) + if n == name@ && y == year as int { delta_of(da@, db@, f) } else { 0 } by {
            lemma_sum_push(before, appended, g_weight(n, y, f));
        }
        if g_keys(before).no_duplicates() {
            assert(!g_keys(before).contains((name@, year as int)));
            verus_prooflib::lemma_unique_push(g_keys(before), (name@, year as int));
            assert(g_keys(rows@) =~= g_keys(before).push((name@, year as int)));
        }
    }
}

pub open spec fn nat_term(t: WMap, name: Seq<u8>, y: int, f: int) -> spec_fn(NationDelta) -> int {
    |nd: NationDelta| if nd.row.n_name@ == name { nd.sign as int * wm_at(t, nd.row.n_nationkey, y, f) } else { 0int }
}

/// g += every nation event's sign times the nation's totals.
fn ingest_nations(g: &mut Vec<GEnt>, t: &WMap, chunk: &[NationDelta])
    ensures
        forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(final(g)@, n, y, f)
            == g_at(old(g)@, n, y, f) + sum(chunk@, nat_term(*t, n, y, f)),
        g_keys(old(g)@).no_duplicates() ==> g_keys(final(g)@).no_duplicates(),
{
    let ghost base = g@;
    let empty: Vec<Ent> = Vec::new();
    let mut i = 0;
    proof {
        assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f) == g_at(base, n, y, f) + sum(chunk@.take(0), nat_term(*t, n, y, f)) by {
            lemma_sum_empty(chunk@.take(0), nat_term(*t, n, y, f));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), base == old(g)@, empty@.len() == 0,
            forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f) == g_at(base, n, y, f) + sum(chunk@.take(i as int), nat_term(*t, n, y, f)),
            g_keys(base).no_duplicates() ==> g_keys(g@).no_duplicates(),
        decreases chunk.len() - i,
    {
        let nd = &chunk[i];
        let rows = bucket(t, nd.row.n_nationkey, &empty);
        let sgn = Wide::from_i128(nd.sign as i128);
        let ghost before = g@;
        let mut j = 0;
        proof {
            assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f)
                == g_at(before, n, y, f) + if n == nd.row.n_name@ { nd.sign as int * ent_at(rows@.take(0), y, f) } else { 0 } by {
                lemma_sum_empty(rows@.take(0), ent_weight(y, f));
                assert(nd.sign as int * 0int == 0) by (nonlinear_arith);
            }
        }
        while j < rows.len()
            invariant j <= rows.len(), before == before, i < chunk.len(), nd == &chunk[i as int], sgn@ == nd.sign as int,
                forall|y: int, f: int| 0 <= f <= 1 ==> #[trigger] ent_at(rows@, y, f) == wm_at(*t, nd.row.n_nationkey, y, f),
                forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f)
                    == g_at(before, n, y, f) + if n == nd.row.n_name@ { nd.sign as int * ent_at(rows@.take(j as int), y, f) } else { 0 },
                g_keys(before).no_duplicates() ==> g_keys(g@).no_duplicates(),
            decreases rows.len() - j,
        {
            let e = &rows[j];
            let da = sgn.mul(&e.a);
            let db = sgn.mul(&e.b);
            let ghost prior = g@;
            bump_g(g, &nd.row.n_name, e.k2, &da, &db);
            proof {
                assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f)
                    == g_at(before, n, y, f) + if n == nd.row.n_name@ { nd.sign as int * ent_at(rows@.take(j as int + 1), y, f) } else { 0 } by {
                    lemma_sum_take(rows@, j as int, ent_weight(y, f));
                    let prev = ent_at(rows@.take(j as int), y, f);
                    let ev = if e.k2 == y { if f == 0 { e.a@ } else { e.b@ } } else { 0int };
                    assert(ent_weight(y, f)(rows@[j as int]) == ev);
                    if n == nd.row.n_name@ {
                        let sg = nd.sign as int;
                        if y == e.k2 as int {
                            assert(sg * (prev + ev) == sg * prev + sg * ev) by (nonlinear_arith);
                        } else {
                            assert(sg * (prev + 0) == sg * prev) by (nonlinear_arith);
                        }
                    }
                }
            }
            j += 1;
        }
        proof {
            assert(rows@.take(rows.len() as int) =~= rows@);
            assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f) == g_at(base, n, y, f) + sum(chunk@.take(i as int + 1), nat_term(*t, n, y, f)) by {
                lemma_sum_take(chunk@, i as int, nat_term(*t, n, y, f));
                assert(nat_term(*t, n, y, f)(chunk@[i as int]) == if n == nd.row.n_name@ { nd.sign as int * wm_at(*t, nd.row.n_nationkey, y, f) } else { 0int });
            }
        }
        i += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}


pub fn groups_from_log(log: &Log<NationDelta>, t: &WMap) -> (g: Vec<GEnt>)
    ensures
        forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f) == sum(log.events(), nat_term(*t, n, y, f)),
        g_keys(g@).no_duplicates(),
{
    let mut g: Vec<GEnt> = Vec::new();
    let mut j = 0;
    proof {
        assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f) == sum(flat(log.parts().take(0)), nat_term(*t, n, y, f)) by {
            assert(flat(log.parts().take(0)) =~= Seq::<NationDelta>::empty());
            lemma_sum_empty(Seq::<NationDelta>::empty(), nat_term(*t, n, y, f));
            lemma_sum_empty(g@, g_weight(n, y, f));
        }
        assert(g_keys(g@) =~= Seq::<(Seq<u8>, int)>::empty());
    }
    while j < log.chunks.len()
        invariant j <= log.chunks.len(),
            forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g@, n, y, f) == sum(flat(log.parts().take(j as int)), nat_term(*t, n, y, f)),
            g_keys(g@).no_duplicates(),
        decreases log.chunks.len() - j,
    {
        ingest_nations(&mut g, t, log.chunks[j].as_slice());
        proof {
            let parts = log.parts();
            assert(parts.take(j as int + 1).drop_last() =~= parts.take(j as int));
            assert(flat(parts.take(j as int + 1)) == flat(parts.take(j as int)) + parts[j as int]);
            assert(parts[j as int] == (*log.chunks@[j as int])@);
            assert forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 implies #[trigger] g_at(g@, n, y, f) == sum(flat(parts.take(j as int + 1)), nat_term(*t, n, y, f)) by {
                lemma_sum_concat(flat(parts.take(j as int)), parts[j as int], nat_term(*t, n, y, f));
            }
        }
        j += 1;
    }
    proof { assert(log.parts().take(j as int) =~= log.parts()); }
    g
}

/// Groups agree with the query's group counts and sums when T agrees with the nation totals.
pub proof fn lemma_groups_are_query(t: WMap, events: Seq<NationDelta>, g: Seq<GEnt>, db: Db, pp: Params)
    requires built_from(db.nation, nation_delta_seq_view(events)),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == nation_total(filter_db(db, pp.p1), nk as int, y, f),
        forall|n: Seq<u8>, y: int, f: int| 0 <= f <= 1 ==> #[trigger] g_at(g, n, y, f) == sum(events, nat_term(t, n, y, f)),
    ensures forall|n: Seq<u8>, y: int| #[trigger] g_at(g, n, y, 0) == group_count(db, pp, n, y)
        && g_at(g, n, y, 1) == group_sum(db, pp, n, y, |row: Tuple| row.profit.amount),
{
    let dbp = filter_db(db, pp.p1);
    crate::meaning::lemma_filter_same(db, pp.p1);
    assert forall|n: Seq<u8>, y: int| #[trigger] g_at(g, n, y, 0) == group_count(db, pp, n, y)
        && g_at(g, n, y, 1) == group_sum(db, pp, n, y, |row: Tuple| row.profit.amount) by {
        assert forall|f: int| 0 <= f <= 1 implies g_at(g, n, y, f) == group_database(dbp, n, y, f) by {
            let h = |row: NationRow| if row.n_name == n { nation_total(dbp, row.n_nationkey, y, f) } else { 0int };
            lemma_built_sum(db.nation, nation_delta_seq_view(events), h);
            assert forall|nd: NationDelta| #[trigger] nat_term(t, n, y, f)(nd) == signed(h)((nation_in_view(nd.row), nd.sign as int)) by {
                assert(wm_at(t, nd.row.n_nationkey, y, f) == nation_total(dbp, nd.row.n_nationkey as int, y, f));
            }
            lemma_sum_map(events, |nd: NationDelta| (nation_in_view(nd.row), nd.sign as int), signed(h), nat_term(t, n, y, f));
            assert(nation_delta_seq_view(events) =~= events.map_values(|nd: NationDelta| (nation_in_view(nd.row), nd.sign as int)));
            lemma_sum_ext_all(db.nation, h, |row: NationRow| if row.n_name == n { nation_total(dbp, row.n_nationkey, y, f) } else { 0int });
            assert(dbp.nation == db.nation);
        }
        crate::query_proofs::lemma_query_groups(db, pp, n, y, 0);
        crate::query_proofs::lemma_query_groups(db, pp, n, y, 1);
    }
}

}
