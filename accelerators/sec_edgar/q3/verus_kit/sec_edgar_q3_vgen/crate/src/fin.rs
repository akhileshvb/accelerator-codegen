// What a read needs of a class: its entries as a list satisfy the facts the spec's choices are built from
// (the companies' rounded totals, their average, the groups' rounded totals), and from them the answer.
use vstd::prelude::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::*;
use verus_prooflib::float::*;
use verus_prooflib::ordered::{Index, KeyView, lemma_ordered_unique};
use verus_prooflib::wide::Wide;
use crate::ent::*;
use crate::f64bits::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::qty::*;

verus! {

pub open spec fn gent_ok(db: Db, p: Params, e: Ent) -> bool {
    &&& e.count@ == group_sum(db, p, opt_bytes(e.name), opt_int(e.cik), tup_vs())
    &&& e.count@ >= 1
    &&& e.sum@ == group_sum(db, p, opt_bytes(e.name), opt_int(e.cik), tup_value())
    &&& e.abs@ == group_sum(db, p, opt_bytes(e.name), opt_int(e.cik), tup_mag())
    &&& rounded_as(e.sum@, e.cv@, e.bits)
    &&& float_within(e.cv@, e.sum@, e.abs@, e.count@ - 1)
}

pub open spec fn gents_ok(db: Db, p: Params, gs: Seq<Ent>) -> bool {
    &&& forall|i: int| 0 <= i < gs.len() ==> gent_ok(db, p, #[trigger] gs[i])
    &&& forall|i: int, j: int| 0 <= i < j < gs.len() ==>
        !(opt_bytes((#[trigger] gs[i]).name) == opt_bytes((#[trigger] gs[j]).name) && gs[i].cik == gs[j].cik)
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| group_sum(db, p, gn, gc, tup_vs()) > 0 ==>
        exists|i: int| 0 <= i < gs.len() && opt_bytes((#[trigger] gs[i]).name) == gn && opt_int(gs[i].cik) == gc
}

pub open spec fn ient_ok(db: Db, p: Params, e: Ent) -> bool {
    &&& e.count@ == avg_sub_group_sum(db, p, opt_int(e.cik), avg_vs())
    &&& e.count@ >= 1
    &&& e.sum@ == avg_sub_group_sum(db, p, opt_int(e.cik), avg_value())
    &&& e.abs@ == avg_sub_group_sum(db, p, opt_int(e.cik), avg_mag())
    &&& float_within(e.cv@, e.sum@, e.abs@, e.count@ - 1)
}

pub open spec fn ients_ok(db: Db, p: Params, es: Seq<Ent>) -> bool {
    &&& forall|i: int| 0 <= i < es.len() ==> ient_ok(db, p, #[trigger] es[i])
    &&& forall|i: int, j: int| 0 <= i < j < es.len() ==> (#[trigger] es[i]).cik != (#[trigger] es[j]).cik
    &&& forall|k: Option<int>| avg_sub_group_sum(db, p, k, avg_vs()) > 0 ==>
        exists|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k
}

pub open spec fn ents_of(m: Index<Ent>) -> Seq<Ent> {
    m.entries().map_values(|row: (KeyView, Ent)| row.1)
}

pub proof fn lemma_gents_of(db: Db, p: Params, m: Index<Ent>)
    requires
        tmap_ok_f(false, m, tf_of(db, p, false)),
        nums_dbl(db),
    ensures gents_ok(db, p, ents_of(m)),
{
    let ents = m.entries();
    m.model();
    lemma_ordered_unique(ents);
    let es = ents_of(m);
    assert forall|i: int| 0 <= i < es.len() implies gent_ok(db, p, #[trigger] es[i]) by {
        let (k, e) = (ents[i].0, ents[i].1);
        assert(es[i] == e);
        assert(m.at(k) == Some(e));
        let (gn, gc) = (opt_bytes(e.name), opt_int(e.cik));
        assert(k == gk(gn, gc));
        assert(ek(false, gn, gc) == k);
        let q = qj(db, p, false, gn, gc);
        lemma_outer_join(db, p, gn, gc);
        lemma_group_facts(db, p, false, gn, gc);
        lemma_ent_within(e, q);
    }
    assert forall|i: int, j: int| 0 <= i < j < es.len() implies
        !(opt_bytes((#[trigger] es[i]).name) == opt_bytes((#[trigger] es[j]).name) && es[i].cik == es[j].cik) by {
        assert(ents[i].0 != ents[j].0);
        let (ei, ej) = (ents[i].1, ents[j].1);
        assert(ents[i].0 == gk(opt_bytes(ei.name), opt_int(ei.cik)));
        assert(ents[j].0 == gk(opt_bytes(ej.name), opt_int(ej.cik)));
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| group_sum(db, p, gn, gc, tup_vs()) > 0 implies
        exists|i: int| 0 <= i < es.len() && opt_bytes((#[trigger] es[i]).name) == gn && opt_int(es[i].cik) == gc by {
        lemma_outer_join(db, p, gn, gc);
        let k = ek(false, gn, gc);
        assert(m.at(k) is Some);
        let i = choose|i: int| 0 <= i < ents.len() && ents[i].0 == k;
        assert(m.at(ents[i].0) == Some(ents[i].1));
        assert(ent_names(false, ents[i].1, gn, gc));
        assert(es[i] == ents[i].1);
    }
}

pub proof fn lemma_ients_of(db: Db, p: Params, m: Index<Ent>)
    requires
        tmap_ok_f(true, m, tf_of(db, p, true)),
        nums_dbl(db),
    ensures ients_ok(db, p, ents_of(m)),
{
    let ents = m.entries();
    m.model();
    lemma_ordered_unique(ents);
    let es = ents_of(m);
    assert forall|i: int| 0 <= i < es.len() implies ient_ok(db, p, #[trigger] es[i]) by {
        let (k, e) = (ents[i].0, ents[i].1);
        assert(es[i] == e);
        assert(m.at(k) == Some(e));
        let gc = opt_int(e.cik);
        assert(k == gk(opt_bytes(e.name), gc));
        assert(e.name is None);
        assert(ek(true, None, gc) == k);
        let q = qj(db, p, true, None, gc);
        lemma_inner_join(db, p, gc);
        lemma_group_facts(db, p, true, None, gc);
        lemma_ent_within(e, q);
    }
    assert forall|i: int, j: int| 0 <= i < j < es.len() implies (#[trigger] es[i]).cik != (#[trigger] es[j]).cik by {
        assert(ents[i].0 != ents[j].0);
        let (ei, ej) = (ents[i].1, ents[j].1);
        assert(ents[i].0 == gk(opt_bytes(ei.name), opt_int(ei.cik)));
        assert(ents[j].0 == gk(opt_bytes(ej.name), opt_int(ej.cik)));
        assert(ei.name is None && ej.name is None);
    }
    assert forall|kk: Option<int>| avg_sub_group_sum(db, p, kk, avg_vs()) > 0 implies
        exists|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == kk by {
        lemma_inner_join(db, p, kk);
        let k = ek(true, None, kk);
        assert(m.at(k) is Some);
        let i = choose|i: int| 0 <= i < ents.len() && ents[i].0 == k;
        assert(m.at(ents[i].0) == Some(ents[i].1));
        assert(ent_names(true, ents[i].1, None, kk));
        assert(es[i] == ents[i].1);
    }
}

}
