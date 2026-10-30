// The class totals the engine's float-overflow check reads: the companies' magnitude and tuple count
// bound every group's magnitude, and are the subquery's sums over its tuples.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::KeyView;
use crate::ent::*;
use crate::fin::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::join_spec::*;
use crate::qty::*;

verus! {

/// ia and n are the magnitude and count of the subquery's tuples; ia is at least any group's magnitude.
pub open spec fn totals_bound(db: Db, p: Params, ia: int, n: int) -> bool {
    &&& ia == sum(avg_sub_tuples(db, p), avg_mag())
    &&& n == sum(avg_sub_tuples(db, p), avg_vs())
    &&& ia >= 0
    &&& n >= 0
    &&& forall|gc: Option<int>| avg_sub_group_sum(db, p, gc, avg_mag()) <= ia
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| group_sum(db, p, gn, gc, tup_mag()) <= ia
}

/// A group's magnitude is within its company's.
proof fn lemma_group_mag_le_company(db: Db, p: Params, gn: Option<Seq<u8>>, gc: Option<int>)
    ensures qj(db, p, false, gn, gc).a <= qj(db, p, true, None, gc).a,
{
    let hout = gh(false, gn, gc, fm());
    let hin = gh(true, None, gc, fm());
    lemma_jq_tuples(db, p, hout);
    lemma_jq_tuples(db, p, hin);
    assert forall|t: Tuple| #[trigger] hout(pj(t)) <= hin(pj(t)) by {}
    lemma_sum_le(tuples(db, p), |tp: Tuple| hout(pj(tp)), |tp: Tuple| hin(pj(tp)));
}

/// Every tuple of the subquery belongs to a company with an entry.
proof fn lemma_tuple_company(db: Db, p: Params, es: Seq<Ent>, i: int)
    requires
        ients_ok(db, p, es),
        0 <= i < avg_sub_tuples(db, p).len(),
    ensures
        exists|j: int| 0 <= j < es.len() && opt_int((#[trigger] es[j]).cik) == avg_sub_tuples(db, p)[i].s2.cik,
{
    let tup = avg_sub_tuples(db, p);
    let k = tup[i].s2.cik;
    assert(tup.contains(tup[i]));
    lemma_avg_tuple_where(db, p, tup[i]);
    let ind = |t: AvgSubTuple| if t.s2.cik == k { avg_vs()(t) } else { 0int };
    assert forall|j: int| 0 <= j < tup.len() implies ind(#[trigger] tup[j]) >= 0 by {}
    lemma_sum_ge_term(tup, ind, i);
    assert(avg_sub_group_sum(db, p, k, avg_vs()) == sum(tup, ind));
}

/// The sum over the subquery's tuples is the sum over the companies' entries.
proof fn lemma_inner_partition(db: Db, p: Params, es: Seq<Ent>, f: spec_fn(AvgSubTuple) -> int, ew: spec_fn(Ent) -> int)
    requires
        ients_ok(db, p, es),
        forall|i: int| 0 <= i < es.len() ==> ew(#[trigger] es[i]) == avg_sub_group_sum(db, p, opt_int(es[i].cik), f),
    ensures
        sum(avg_sub_tuples(db, p), f) == sum(es, ew),
{
    let tup = avg_sub_tuples(db, p);
    let gk_ = |e: Ent| opt_int(e.cik);
    let ekeys = es.map_values(gk_);
    let key = |t: AvgSubTuple| t.s2.cik;
    let per = |k: Option<int>| avg_sub_group_sum(db, p, k, f);
    assert(distinct(ekeys)) by {
        assert forall|i: int, j: int| 0 <= i < ekeys.len() && 0 <= j < ekeys.len() && i != j implies ekeys[i] != ekeys[j] by {
            if i < j { assert(es[i].cik != es[j].cik); } else { assert(es[j].cik != es[i].cik); }
        }
    }
    assert forall|i: int| 0 <= i < tup.len() && f(tup[i]) != 0 implies ekeys.contains(key(#[trigger] tup[i])) by {
        lemma_tuple_company(db, p, es, i);
        let j = choose|j: int| 0 <= j < es.len() && opt_int((#[trigger] es[j]).cik) == tup[i].s2.cik;
        assert(ekeys[j] == key(tup[i]));
    }
    assert forall|k: Option<int>| #[trigger] per(k) == sum(tup, |x: AvgSubTuple| if key(x) == k { f(x) } else { 0 }) by {
        lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == k { f(t) } else { 0 }, |x: AvgSubTuple| if key(x) == k { f(x) } else { 0 });
    }
    lemma_sum_group(tup, ekeys, key, f, per);
    let hh = |e: Ent| per(opt_int(e.cik));
    lemma_sum_map(es, gk_, per, hh);
    assert forall|i: int| 0 <= i < es.len() implies hh(#[trigger] es[i]) == ew(es[i]) by {}
    lemma_sum_ext(es, hh, ew);
}

/// A class's totals bound the groups' magnitudes.
pub proof fn lemma_class_totals(db: Db, p: Params, cls: Class)
    requires
        cls_ok(cls, db, p),
        nums_dbl(db),
    ensures
        totals_bound(db, p, cls.ia@, cls.tn@),
{
    let es = ents_of(cls.cmap);
    let tup = avg_sub_tuples(db, p);
    lemma_ients_of(db, p, cls.cmap);
    lemma_sum_map(cls.cmap.entries(), |row: (KeyView, Ent)| row.1, |e: Ent| e.abs@, aw());
    lemma_sum_map(cls.cmap.entries(), |row: (KeyView, Ent)| row.1, |e: Ent| e.count@, nw());
    assert(cls.ia@ == sum(es, |e: Ent| e.abs@));
    assert(cls.tn@ == sum(es, |e: Ent| e.count@));
    lemma_inner_partition(db, p, es, avg_mag(), |e: Ent| e.abs@);
    lemma_inner_partition(db, p, es, avg_vs(), |e: Ent| e.count@);
    assert forall|i: int| 0 <= i < tup.len() implies avg_mag()(#[trigger] tup[i]) >= 0 && avg_vs()(tup[i]) >= 0 by {}
    lemma_sum_nonneg(tup, avg_mag());
    lemma_sum_nonneg(tup, avg_vs());
    assert forall|gc: Option<int>| avg_sub_group_sum(db, p, gc, avg_mag()) <= cls.ia@ by {
        lemma_sum_le(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_mag()(t) } else { 0 }, avg_mag());
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| group_sum(db, p, gn, gc, tup_mag()) <= cls.ia@ by {
        lemma_outer_join(db, p, gn, gc);
        lemma_group_mag_le_company(db, p, gn, gc);
        lemma_inner_join(db, p, gc);
        lemma_qj_norm(db, p, true, None, gc, gn);
        assert(avg_sub_group_sum(db, p, gc, avg_mag()) <= cls.ia@);
    }
}

}
