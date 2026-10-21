// The general path: the answer for any pattern, computed exactly from the event logs.
use vstd::prelude::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::wmap::*;
use crate::ingest::*;
use crate::linepass::*;
use crate::groups::*;
use crate::emitw::*;
use crate::raw::*;

verus! {

pub fn slow_answer(raw: &Raw, p1: &[u8], Ghost(db): Ghost<Db>) -> (r: Option<Vec<(Vec<u8>, i64, i128)>>)
    requires raw_of(*raw, db),
    ensures
        r matches Some(out) ==> query_result(db, params(p1@), out_view(out@)),
        r is None ==> !answer_fits(db, params(p1@)),
{
    let d = dates_from_log(&raw.orders);
    let s = sup_from_log(&raw.suppliers);
    let ps = ps_from_log(&raw.partsupp);
    let g = gr_from_log(&raw.parts, p1);
    let t = t_from_log(&raw.lines, &d, &s, &ps, &g);
    let groups = groups_from_log(&raw.nations, &t);
    proof {
        lemma_dates_index_is_dates(d, raw.orders.events(), db);
        lemma_sup_index_is_count(s, raw.suppliers.events(), db);
        lemma_ps_index_is_pairs(ps, raw.partsupp.events(), db);
        lemma_gr_index_is_green(g, raw.parts.events(), p1@, db);
        lemma_t_is_nation_total(t, d, s, ps, g, raw.lines.events(), db, p1@);
        lemma_groups_are_query(t, raw.nations.events(), groups@, db, params(p1@));
    }
    emit_groups(&groups, Ghost(db), Ghost(params(p1@)))
}

}
