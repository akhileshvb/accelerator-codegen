// The general path's answer: the groups with tuples, sorted, or None when a profit does not
// fit the result's type (then no correct answer fits either).
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use crate::output::*;
use crate::aggregate::sort_rows;
use crate::wide::*;
use crate::groups::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn g_row(e: GEnt) -> OutRow {
    OutRow { nation: e.name@, o_year: e.year as int, sum_profit: e.b@ }
}

/// The rows of the groups that have tuples.
pub open spec fn g_rows(es: Seq<GEnt>) -> Seq<OutRow>
    decreases es.len(),
{
    if es.len() == 0 { Seq::empty() } else {
        let rest = g_rows(es.drop_last());
        if es.last().a@ > 0 { rest.push(g_row(es.last())) } else { rest }
    }
}

proof fn lemma_g_rows_contains(es: Seq<GEnt>, row: OutRow)
    ensures g_rows(es).contains(row) <==> exists|k: int| 0 <= k < es.len() && es[k].a@ > 0 && g_row(es[k]) == row,
    decreases es.len(),
{
    if es.len() > 0 {
        lemma_g_rows_contains(es.drop_last(), row);
        let rest = g_rows(es.drop_last());
        if es.last().a@ > 0 {
            assert(g_rows(es) == rest.push(g_row(es.last())));
            if g_rows(es).contains(row) && !rest.contains(row) {
                assert(row == g_row(es.last()));
                assert(es[es.len() - 1] == es.last());
            }
            if rest.contains(row) {
                let k = choose|k: int| 0 <= k < es.drop_last().len() && es.drop_last()[k].a@ > 0 && g_row(es.drop_last()[k]) == row;
                assert(es[k] == es.drop_last()[k]);
            }
            if exists|k: int| 0 <= k < es.len() && es[k].a@ > 0 && g_row(es[k]) == row {
                let k = choose|k: int| 0 <= k < es.len() && es[k].a@ > 0 && g_row(es[k]) == row;
                if k < es.len() - 1 { assert(es.drop_last()[k] == es[k]); }
                else { assert(es[k] == es.last()); }
            }
        } else {
            assert(g_rows(es) == rest);
            if exists|k: int| 0 <= k < es.len() && es[k].a@ > 0 && g_row(es[k]) == row {
                let k = choose|k: int| 0 <= k < es.len() && es[k].a@ > 0 && g_row(es[k]) == row;
                assert(k < es.len() - 1);
                assert(es.drop_last()[k] == es[k]);
            }
            if rest.contains(row) {
                let k = choose|k: int| 0 <= k < es.drop_last().len() && es.drop_last()[k].a@ > 0 && g_row(es.drop_last()[k]) == row;
                assert(es[k] == es.drop_last()[k]);
            }
        }
    }
}

proof fn lemma_g_rows_unique(es: Seq<GEnt>)
    requires g_keys(es).no_duplicates(),
    ensures g_rows(es).map_values(|row: OutRow| output_key(row)).no_duplicates(),
    decreases es.len(),
{
    if es.len() > 0 {
        let pre = es.drop_last();
        assert(g_keys(pre) =~= g_keys(es).drop_last());
        lemma_g_rows_unique(pre);
        let rest = g_rows(pre);
        if es.last().a@ > 0 {
            let keys = rest.map_values(|row: OutRow| output_key(row));
            let x = g_row(es.last());
            assert forall|k: int| 0 <= k < rest.len() implies keys[k] != output_key(x) by {
                lemma_g_rows_contains(pre, rest[k]);
                assert(rest.contains(rest[k]));
                let j = choose|j: int| 0 <= j < pre.len() && pre[j].a@ > 0 && g_row(pre[j]) == rest[k];
                assert(g_keys(es)[j] == (pre[j].name@, pre[j].year as int));
                assert(g_keys(es)[es.len() - 1] == (es.last().name@, es.last().year as int));
            }
            assert(!keys.contains(output_key(x)));
            verus_prooflib::lemma_unique_push(keys, output_key(x));
            assert(g_rows(es).map_values(|row: OutRow| output_key(row)) =~= keys.push(output_key(x)));
        } else {
            assert(g_rows(es) == rest);
        }
    }
}

/// With distinct keys, a group's entry is that group's total.
proof fn lemma_g_single(es: Seq<GEnt>, k: int, f: int)
    requires g_keys(es).no_duplicates(), 0 <= k < es.len(), 0 <= f <= 1,
    ensures g_at(es, es[k].name@, es[k].year as int, f) == if f == 0 { es[k].a@ } else { es[k].b@ },
{
    let name = es[k].name@;
    let year = es[k].year as int;
    assert forall|i: int| 0 <= i < es.len() && i != k implies g_weight(name, year, f)(#[trigger] es[i]) == 0 by {
        assert(g_keys(es)[i] != g_keys(es)[k]);
    }
    lemma_sum_single_match(es, g_weight(name, year, f), k);
}

/// An entry with no tuples has no profit either (the totals come from a database).
proof fn lemma_g_missing(es: Seq<GEnt>, name: Seq<u8>, year: int, f: int)
    requires forall|k: int| 0 <= k < es.len() ==> !(es[k].name@ == name && es[k].year == year),
    ensures g_at(es, name, year, f) == 0,
{
    lemma_sum_zero(es, g_weight(name, year, f));
}

pub open spec fn groups_match(g: Seq<GEnt>, db: Db, pp: Params) -> bool {
    g_keys(g).no_duplicates() && forall|n: Seq<u8>, y: int| #[trigger] g_at(g, n, y, 0) == group_count(db, pp, n, y)
        && g_at(g, n, y, 1) == group_sum(db, pp, n, y, |row: Tuple| row.profit.amount)
}

pub fn emit_groups(g: &Vec<GEnt>, Ghost(db): Ghost<Db>, Ghost(pp): Ghost<Params>) -> (r: Option<Vec<(Vec<u8>, i64, i128)>>)
    requires groups_match(g@, db, pp),
    ensures
        r matches Some(out) ==> query_result(db, pp, out_view(out@)),
        r is None ==> !answer_fits(db, pp),
{
    let mut out: Vec<(Vec<u8>, i64, i128)> = Vec::new();
    let mut i = 0;
    while i < g.len()
        invariant i <= g.len(), groups_match(g@, db, pp), out_view(out@) == g_rows(g@.take(i as int)),
        decreases g.len() - i,
    {
        let e = &g[i];
        proof {
            assert(g@.take(i as int + 1).drop_last() =~= g@.take(i as int));
            assert(g@.take(i as int + 1).last() == g@[i as int]);
        }
        if e.a.is_pos() {
            match e.b.to_i128() {
                Some(v) => {
                    out.push((copy_bytes(&e.name), e.year, v));
                    proof {
                        assert(out_view(out@) =~= g_rows(g@.take(i as int)).push(g_row(g@[i as int])));
                    }
                }
                None => {
                    proof {
                        lemma_g_single(g@, i as int, 0);
                        lemma_g_single(g@, i as int, 1);
                        let name = e.name@;
                        let year = e.year as int;
                        assert(group_count(db, pp, name, year) > 0);
                        lemma_answer_ok(db, pp);
                        let o = answer(db, pp);
                        let idx = choose|idx: int| 0 <= idx < o.len() && (#[trigger] o[idx]).nation == name && o[idx].o_year == year;
                        assert(row_ok(db, pp, o[idx]));
                        assert(!out_fits(o[idx]));
                        assert(query_result(db, pp, o));
                    }
                    return None;
                }
            }
        }
        i += 1;
    }
    proof {
        assert(g@.take(i as int) =~= g@);
        lemma_g_rows_unique(g@);
    }
    let ghost before = out_view(out@);
    let sorted = sort_rows(out);
    proof {
        let rows = g_rows(g@);
        assert forall|k: int| 0 <= k < out_view(sorted@).len() implies row_ok(db, pp, #[trigger] out_view(sorted@)[k]) by {
            let row = out_view(sorted@)[k];
            assert(out_view(sorted@).contains(row));
            assert(rows.contains(row));
            lemma_g_rows_contains(g@, row);
            let j = choose|j: int| 0 <= j < g.len() && g@[j].a@ > 0 && g_row(g@[j]) == row;
            lemma_g_single(g@, j, 0);
            lemma_g_single(g@, j, 1);
        }
        assert forall|n: Seq<u8>, y: int| #[trigger] group_count(db, pp, n, y) > 0 && having(db, pp, n, y) implies
            exists|k: int| 0 <= k < out_view(sorted@).len() && (#[trigger] out_view(sorted@)[k]).nation == n && out_view(sorted@)[k].o_year == y by {
            if !exists|j: int| 0 <= j < g.len() && g@[j].name@ == n && g@[j].year == y {
                lemma_g_missing(g@, n, y, 0);
            }
            let j = choose|j: int| 0 <= j < g.len() && g@[j].name@ == n && g@[j].year == y;
            lemma_g_single(g@, j, 0);
            lemma_g_rows_contains(g@, g_row(g@[j]));
            assert(out_view(sorted@).contains(g_row(g@[j])));
            let k = choose|k: int| 0 <= k < out_view(sorted@).len() && out_view(sorted@)[k] == g_row(g@[j]);
        }
    }
    Some(sorted)
}

}
