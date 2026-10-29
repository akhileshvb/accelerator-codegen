// Stage B of a commit: the batch's accumulated changes merged into the state's maps, group by group,
// and the changed groups' totals rounded again.  While the merge runs, a group the batch touches has
// the quantities of the database before the commit until it is merged, those after it from then on.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::ordered::{lemma_ordered_unique, lemma_sum_put, lemma_sum_remove, contribution};
use verus_prooflib::wide::Wide;
use crate::dmap::*;
use crate::ent::*;
use crate::exact::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::numeric::*;
use crate::qty::*;
use crate::rank::*;

verus! {

/// The truth during a merge: the groups of the first i entries are merged.
pub open spec fn tf_mid(db: Db, db2: Db, p: Params, inner: bool, ents: Seq<(KeyView, Ent)>, i: int) -> spec_fn(Option<Seq<u8>>, Option<int>) -> Q {
    |gn: Option<Seq<u8>>, gc: Option<int>|
        if exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == ek(inner, gn, gc) { qj(db2, p, inner, gn, gc) } else { qj(db, p, inner, gn, gc) }
}

proof fn lemma_tmap_tf(inner: bool, m: Index<Ent>, tf1: spec_fn(Option<Seq<u8>>, Option<int>) -> Q, tf2: spec_fn(Option<Seq<u8>>, Option<int>) -> Q)
    requires
        tmap_ok_f(inner, m, tf1),
        forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] tf1(gn, gc) == tf2(gn, gc),
    ensures tmap_ok_f(inner, m, tf2),
{
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
        implies ent_ok(m.at(ek(inner, gn, gc))->0, tf2(gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc) by {
        assert(tf1(gn, gc) == tf2(gn, gc));
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
        implies tf2(gn, gc).n == 0 by {
        assert(tf1(gn, gc) == tf2(gn, gc));
    }
}

/// One entry merged: the map changes at one key, to the entry for the database after the commit.
proof fn lemma_merge_step(inner: bool, before: Index<Ent>, after: Index<Ent>, db: Db, db2: Db, p: Params, ents: Seq<(KeyView, Ent)>, i: int,
    gn_d: Option<Seq<u8>>, gc_d: Option<int>, new_entry: Option<Ent>)
    requires
        0 <= i < ents.len(),
        tmap_ok_f(inner, before, tf_mid(db, db2, p, inner, ents, i)),
        ents[i].0 == ek(inner, gn_d, gc_d),
        forall|a: int, b: int| 0 <= a < b < ents.len() ==> (#[trigger] ents[a]).0 != (#[trigger] ents[b]).0,
        after.wf(),
        forall|q: KeyView| #[trigger] after.at(q) == if q == ents[i].0 { new_entry } else { before.at(q) },
        match new_entry {
            Some(e) => ent_ok(e, qj(db2, p, inner, gn_d, gc_d)) && ent_names(inner, e, gn_d, gc_d)
                && ents[i].0 == gk(opt_bytes(e.name), opt_int(e.cik)) && (inner ==> e.name is None),
            None => qj(db2, p, inner, gn_d, gc_d).n == 0,
        },
    ensures
        tmap_ok_f(inner, after, tf_mid(db, db2, p, inner, ents, i + 1)),
{
    let k = ents[i].0;
    let tf0 = tf_mid(db, db2, p, inner, ents, i);
    let tf1 = tf_mid(db, db2, p, inner, ents, i + 1);
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] tf1(gn, gc) == if ek(inner, gn, gc) == k { qj(db2, p, inner, gn, gc) } else { tf0(gn, gc) } by {
        if ek(inner, gn, gc) == k {
            assert(exists|j: int| 0 <= j < i + 1 && (#[trigger] ents[j]).0 == ek(inner, gn, gc));
        } else {
            if exists|j: int| 0 <= j < i + 1 && (#[trigger] ents[j]).0 == ek(inner, gn, gc) {
                let j = choose|j: int| 0 <= j < i + 1 && (#[trigger] ents[j]).0 == ek(inner, gn, gc);
                assert(j < i);
            }
            if exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == ek(inner, gn, gc) {
                let j = choose|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == ek(inner, gn, gc);
                assert(0 <= j < i + 1);
            }
        }
    }
    // the merged key was not merged before
    assert(!exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == k) by {
        if exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == k {
            let j = choose|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == k;
            assert(ents[j].0 != ents[i].0);
        }
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] after.at(ek(inner, gn, gc)) is Some
        implies ent_ok(after.at(ek(inner, gn, gc))->0, tf1(gn, gc)) && ent_names(inner, after.at(ek(inner, gn, gc))->0, gn, gc) by {
        if ek(inner, gn, gc) == k {
            lemma_gk_inj(if inner { None } else { gn }, gc, if inner { None } else { gn_d }, gc_d);
            lemma_qj_norm(db2, p, inner, gn, gc, gn_d);
            assert(after.at(k) == new_entry);
        } else {
            assert(after.at(ek(inner, gn, gc)) == before.at(ek(inner, gn, gc)));
        }
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] after.at(ek(inner, gn, gc)) is None
        implies tf1(gn, gc).n == 0 by {
        if ek(inner, gn, gc) == k {
            lemma_gk_inj(if inner { None } else { gn }, gc, if inner { None } else { gn_d }, gc_d);
            lemma_qj_norm(db2, p, inner, gn, gc, gn_d);
            assert(after.at(k) == new_entry);
        } else {
            assert(after.at(ek(inner, gn, gc)) == before.at(ek(inner, gn, gc)));
        }
    }
    assert forall|q: KeyView| #[trigger] after.at(q) is Some implies
        q == gk(opt_bytes(after.at(q)->0.name), opt_int(after.at(q)->0.cik)) && (inner ==> after.at(q)->0.name is None) by {
        if q != k { assert(after.at(q) == before.at(q)); }
    }
}

/// Merge the batch's entries of one map into the state's.  With `track`, the map's rounded totals are summed.
#[verifier::spinoff_prover]
pub fn merge_map(inner: bool, track: bool, track_o: bool, m: &mut Index<Ent>, dm: &Index<Ent>, sr: &mut Wide, kk: &mut Wide, ord: &mut Index<Ent>, oor: &mut Wide, c: &Consts,
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(p): Ghost<Params>, Ghost(l): Ghost<Seq<Ct>>)
    requires
        consts_ok(*c),
        tmap_ok_f(inner, *old(m), tf_of(db, p, inner)),
        dmap_ok(inner, *dm, l, p),
        ledger_ok(l, db, db2),
        nums_dbl(db),
        nums_dbl(db2),
        track ==> old(sr)@ == sum(old(m).entries(), cvw()) && old(kk)@ == sum(old(m).entries(), onew()),
        track_o ==> ord_ok(*old(m), *old(ord)) && old(oor)@ == sum(old(m).entries(), oorw()),
    ensures
        tmap_ok_f(inner, *final(m), tf_of(db2, p, inner)),
        track ==> final(sr)@ == sum(final(m).entries(), cvw()) && final(kk)@ == sum(final(m).entries(), onew()),
        track_o ==> ord_ok(*final(m), *final(ord)) && final(oor)@ == sum(final(m).entries(), oorw()),
        !track_o ==> *final(ord) == *old(ord) && final(oor)@ == old(oor)@,
{
    let rows = dm.rows();
    let ghost ents = dm.entries();
    proof {
        dm.model();
        lemma_ordered_unique(ents);
        assert(tf_of(db, p, inner) == tf_mid(db, db2, p, inner, ents, 0)) by {
            assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] tf_of(db, p, inner)(gn, gc) == tf_mid(db, db2, p, inner, ents, 0)(gn, gc) by {}
        }
        lemma_tmap_tf(inner, *m, tf_of(db, p, inner), tf_mid(db, db2, p, inner, ents, 0));
    }
    let one = Wide::from_i128(1);
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            consts_ok(*c),
            dmap_ok(inner, *dm, l, p),
            ledger_ok(l, db, db2),
            nums_dbl(db),
            nums_dbl(db2),
            one@ == 1,
            rows@.map_values(|row: (Key, Arc<Ent>)| (row.0.view(), *row.1)) == ents,
            forall|a: int, b: int| 0 <= a < b < ents.len() ==> (#[trigger] ents[a]).0 != (#[trigger] ents[b]).0,
            forall|pos: int| #![trigger ents[pos]] 0 <= pos < ents.len() ==> dm.at(ents[pos].0) == Some(ents[pos].1),
            tmap_ok_f(inner, *m, tf_mid(db, db2, p, inner, ents, i as int)),
            track ==> sr@ == sum(m.entries(), cvw()) && kk@ == sum(m.entries(), onew()),
            track_o ==> ord_ok(*m, *ord) && oor@ == sum(m.entries(), oorw()),
            !track_o ==> *ord == *old(ord) && oor@ == old(oor)@,
        decreases rows.len() - i,
    {
        let key = &rows[i].0;
        let d = &*rows[i].1;
        let ghost gn_d = if inner { None::<Seq<u8>> } else { opt_bytes(d.name) };
        let ghost gc_d = opt_int(d.cik);
        let ghost t1 = qj(db, p, inner, gn_d, gc_d);
        let ghost t2 = qj(db2, p, inner, gn_d, gc_d);
        let ghost dqv = dq(l, p, inner, gn_d, gc_d);
        proof {
            assert(ents[i as int] == (key.view(), *d));
            assert(dm.at(key.view()) == Some(*d));
            assert(key.view() == gk(opt_bytes(d.name), opt_int(d.cik)));
            assert(key.view() == ek(inner, gn_d, gc_d)) by {
                if inner { assert(d.name is None); assert(opt_bytes(d.name) == None::<Seq<u8>>); }
            }
            assert(dent_ok(*d, dqv)) by {
                assert(dm.at(ek(inner, gn_d, gc_d)) is Some);
            }
            lemma_ledger_q(l, db, db2, p, inner, gn_d, gc_d);
            lemma_group_facts(db, p, inner, gn_d, gc_d);
            lemma_group_facts(db2, p, inner, gn_d, gc_d);
            assert(!exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == ek(inner, gn_d, gc_d)) by {
                if exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == ek(inner, gn_d, gc_d) {
                    let j = choose|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == ek(inner, gn_d, gc_d);
                    assert(ents[j].0 != ents[i as int].0);
                }
            }
            assert(tf_mid(db, db2, p, inner, ents, i as int)(gn_d, gc_d) == t1);
        }
        let found = m.get(key);
        let ghost before = *m;
        let ghost ord_before = *ord;
        let ghost old_opt: Option<Ent> = before.at(key.view());
        let (c0, s0, a0) = match &found {
            Some(o) => (o.count.add(&d.count), o.sum.add(&d.sum), o.abs.add(&d.abs)),
            None => (d.count.dup(), d.sum.dup(), d.abs.dup()),
        };
        proof {
            match &found {
                Some(o) => {
                    assert(before.at(key.view()) == Some(**o));
                    assert(ent_ok(**o, t1));
                },
                None => {
                    assert(before.at(key.view()) is None);
                    assert(t1.n == 0);
                },
            }
            assert(c0@ == t2.n && s0@ == t2.s && a0@ == t2.a);
        }
        if c0.is_zero() {
            match &found {
                Some(o) => {
                    m.remove(key);
                    if track {
                        *sr = sr.sub(&o.cv);
                        *kk = kk.sub(&one);
                    }
                    if track_o {
                        proof { assert(before.at(key.view()) == Some(**o)); lemma_ent_finite(old_opt->0, t1); }
                        let ok = ord_key(&**o);
                        ord.remove(&ok);
                        if o.bits.is_none() { *oor = oor.sub(&one); }
                    }
                },
                None => {},
            }
            proof {
                lemma_merge_step(inner, before, *m, db, db2, p, ents, i as int, gn_d, gc_d, None);
                if track {
                    if found is Some {
                        lemma_sum_remove(&before, &*m, key.view(), cvw());
                        lemma_sum_remove(&before, &*m, key.view(), onew());
                    } else {
                        assert(m.entries() == before.entries());
                    }
                }
                if track_o {
                    if found is Some {
                        lemma_sum_remove(&before, &*m, key.view(), oorw());
                        assert forall|q: KeyView| #[trigger] ord.at(q) == if old_opt is Some && q == okey(old_opt->0) { None::<Ent> } else { ord_before.at(q) } by {}
                        lemma_ord_step(before, *m, ord_before, *ord, key.view(), old_opt, None);
                    } else {
                        assert(m.entries() == before.entries());
                        assert forall|q: KeyView| #[trigger] ord.at(q) == ord_before.at(q) by {}
                        lemma_ord_step(before, *m, ord_before, *ord, key.view(), None, None);
                    }
                }
            }
        } else {
            let e = ent_make(if inner { None } else { copy_name(&d.name) }, d.cik, c0, s0, a0, c, Ghost(t2));
            let ecv = e.cv.dup();
            let none_bits = e.bits.is_none();
            proof {
                assert(opt_bytes(e.name) == (if inner { None::<Seq<u8>> } else { opt_bytes(d.name) }));
                assert(ent_names(inner, e, gn_d, gc_d));
            }
            let ghost newe = e;
            let arc = Arc::new(e);
            m.put(key.share(), arc.clone());
            if track_o {
                proof { assert(ent_ok(newe, t2)); }
                if let Some(o) = &found {
                    proof { assert(before.at(key.view()) == Some(**o)); lemma_ent_finite(old_opt->0, t1); }
                    let ok = ord_key(&**o);
                    ord.remove(&ok);
                    if o.bits.is_none() { *oor = oor.sub(&one); }
                }
                let nk = ord_key(&*arc);
                ord.put(nk, arc);
                if none_bits { *oor = oor.add(&one); }
            }
            if track {
                let base = match &found {
                    Some(o) => sr.sub(&o.cv),
                    None => sr.dup(),
                };
                *sr = base.add(&ecv);
                if found.is_none() { *kk = kk.add(&one); }
            }
            proof {
                lemma_merge_step(inner, before, *m, db, db2, p, ents, i as int, gn_d, gc_d, Some(newe));
                if track {
                    lemma_sum_put(&before, &*m, key.view(), newe, cvw());
                    lemma_sum_put(&before, &*m, key.view(), newe, onew());
                    assert(cvw()((key.view(), newe)) == newe.cv@);
                }
                if track_o {
                    lemma_sum_put(&before, &*m, key.view(), newe, oorw());
                    assert forall|q: KeyView| #[trigger] ord.at(q) == if q == okey(newe) { Some(newe) }
                        else if old_opt is Some && q == okey(old_opt->0) { None::<Ent> } else { ord_before.at(q) } by {}
                    lemma_ord_step(before, *m, ord_before, *ord, key.view(), old_opt, Some(newe));
                }
            }
        }
        i += 1;
    }
    proof {
        assert(i == ents.len());
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] tf_mid(db, db2, p, inner, ents, ents.len() as int)(gn, gc) == tf_of(db2, p, inner)(gn, gc) by {
            if !exists|j: int| 0 <= j < ents.len() && (#[trigger] ents[j]).0 == ek(inner, gn, gc) {
                assert(dm.at(ek(inner, gn, gc)) is None) by {
                    if dm.at(ek(inner, gn, gc)) is Some {
                        let k = ek(inner, gn, gc);
                        dm.map_model();
                        assert(exists|j: int| 0 <= j < ents.len() && (#[trigger] ents[j]).0 == k);
                    }
                }
                assert forall|x: int| 0 <= x < l.len() implies !contrib_to(inner, #[trigger] l[x], p, gn, gc) by {}
                lemma_dq_zero(l, p, inner, gn, gc);
                lemma_ledger_q(l, db, db2, p, inner, gn, gc);
            }
        }
        lemma_tmap_tf(inner, *m, tf_mid(db, db2, p, inner, ents, ents.len() as int), tf_of(db2, p, inner));
    }
}


proof fn lemma_tmap_same(inner: bool, m1: &Index<Ent>, m2: &Index<Ent>, tf: spec_fn(Option<Seq<u8>>, Option<int>) -> Q)
    requires
        tmap_ok_f(inner, *m1, tf),
        m2.wf(),
        forall|k: KeyView| #[trigger] m1.at(k) == m2.at(k),
    ensures tmap_ok_f(inner, *m2, tf),
{
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m2.at(ek(inner, gn, gc)) is Some
        implies ent_ok(m2.at(ek(inner, gn, gc))->0, tf(gn, gc)) && ent_names(inner, m2.at(ek(inner, gn, gc))->0, gn, gc) by {
        assert(m1.at(ek(inner, gn, gc)) == m2.at(ek(inner, gn, gc)));
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m2.at(ek(inner, gn, gc)) is None
        implies tf(gn, gc).n == 0 by {
        assert(m1.at(ek(inner, gn, gc)) == m2.at(ek(inner, gn, gc)));
    }
    assert forall|k: KeyView| #[trigger] m2.at(k) is Some implies
        k == gk(opt_bytes(m2.at(k)->0.name), opt_int(m2.at(k)->0.cik)) && (inner ==> m2.at(k)->0.name is None) by {
        assert(m1.at(k) == m2.at(k));
    }
}

pub proof fn lemma_empty_tmap(inner: bool, m: Index<Ent>, tf: spec_fn(Option<Seq<u8>>, Option<int>) -> Q)
    requires
        m.wf(),
        forall|k: KeyView| #[trigger] m.at(k) is None,
        forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] tf(gn, gc).n == 0,
    ensures tmap_ok_f(inner, m, tf),
{
}

proof fn lemma_no_entries(m: Index<Ent>)
    requires m.wf(), m.entries().len() == 0,
    ensures forall|k: KeyView| #[trigger] m.at(k) is None,
{
    m.model();
    assert forall|k: KeyView| #[trigger] m.at(k) is None by {
        if m.at(k) is Some {
            assert(exists|position: int| 0 <= position < m.entries().len() && m.entries()[position].0 == k);
        }
    }
}

/// Merge the batch's changes of one class into its state.
#[verifier::spinoff_prover]
#[verifier::rlimit(100)]
pub fn merge_class(old: Option<Arc<Class>>, dc: &DCls, c: &Consts,
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(p): Ghost<Params>, Ghost(l): Ghost<Seq<Ct>>) -> (r: Option<Class>)
    requires
        consts_ok(*c),
        match old { Some(oc) => cls_ok(*oc, db, p), None => zero_p(db, p) },
        dmap_ok(true, dc.cd, l, p),
        dmap_ok(false, dc.od, l, p),
        ledger_ok(l, db, db2),
        nums_dbl(db),
        nums_dbl(db2),
    ensures
        match r { Some(cl) => cls_ok(cl, db2, p), None => zero_p(db2, p) },
{
    let (mut cmap, mut omap, mut sr, mut kk, mut ord, mut oor) = match &old {
        Some(oc) => (oc.cmap.share(), oc.omap.share(), oc.sr.dup(), oc.k.dup(), oc.ord.share(), oc.oor.dup()),
        None => (Index::new(), Index::new(), Wide::zero(), Wide::zero(), Index::new(), Wide::zero()),
    };
    proof {
        match &old {
            Some(oc) => {
                lemma_tmap_same(true, &oc.cmap, &cmap, tf_of(db, p, true));
                lemma_tmap_same(false, &oc.omap, &omap, tf_of(db, p, false));
                lemma_ord_same(&oc.omap, &omap, &oc.ord, &ord);
                assert(omap.entries() == oc.omap.entries());
            },
            None => {
                lemma_empty_tmap(true, cmap, tf_of(db, p, true));
                lemma_empty_tmap(false, omap, tf_of(db, p, false));
                assert(cmap.entries() =~= Seq::<(KeyView, Ent)>::empty());
                lemma_sum_empty(cmap.entries(), cvw());
                lemma_sum_empty(cmap.entries(), onew());
                assert(omap.entries() =~= Seq::<(KeyView, Ent)>::empty());
                lemma_sum_empty(omap.entries(), oorw());
                assert(ord_ok(omap, ord));
            },
        }
    }
    merge_map(true, true, false, &mut cmap, &dc.cd, &mut sr, &mut kk, &mut ord, &mut oor, c, Ghost(db), Ghost(db2), Ghost(p), Ghost(l));
    let mut d1 = Wide::zero();
    let mut d2 = Wide::zero();
    merge_map(false, false, true, &mut omap, &dc.od, &mut d1, &mut d2, &mut ord, &mut oor, c, Ghost(db), Ghost(db2), Ghost(p), Ghost(l));
    if cmap.is_empty() && omap.is_empty() {
        proof {
            lemma_no_entries(cmap);
            lemma_no_entries(omap);
            assert forall|gn: Option<Seq<u8>>, gc: Option<int>| qj(db2, p, true, gn, gc).n == 0 && qj(db2, p, false, gn, gc).n == 0 by {
                assert(cmap.at(ek(true, gn, gc)) is None);
                assert(omap.at(ek(false, gn, gc)) is None);
            }
        }
        None
    } else {
        Some(Class { cmap, omap, sr, k: kk, ord, oor })
    }
}

/// The classes during a merge: those of the first i entries are merged.
pub open spec fn dsel(db: Db, db2: Db, ents: Seq<(KeyView, DCls)>, i: int, k: KeyView) -> Db {
    if exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == k { db2 } else { db }
}

pub open spec fn agg_ok_sel(cs: Index<Class>, db: Db, db2: Db, ents: Seq<(KeyView, DCls)>, i: int) -> bool {
    &&& cs.wf()
    &&& forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is Some ==>
        cls_ok(cs.at(ckey(fy as int, uom))->0, dsel(db, db2, ents, i, ckey(fy as int, uom)), cp(fy, uom))
    &&& forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is None ==>
        zero_p(dsel(db, db2, ents, i, ckey(fy as int, uom)), cp(fy, uom))
}

proof fn lemma_cls_same(c: Class, db: Db, db2: Db, p: Params)
    requires
        cls_ok(c, db, p),
        forall|inner: bool, gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] qj(db2, p, inner, gn, gc) == qj(db, p, inner, gn, gc),
    ensures cls_ok(c, db2, p),
{
    lemma_tmap_tf(true, c.cmap, tf_of(db, p, true), tf_of(db2, p, true));
    lemma_tmap_tf(false, c.omap, tf_of(db, p, false), tf_of(db2, p, false));
}

/// Merge the batch's changes into all the state's classes.
#[verifier::spinoff_prover]
pub fn merge_all(cs: &mut Index<Class>, dm: &Index<DCls>, c: &Consts, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(l): Ghost<Seq<Ct>>)
    requires
        consts_ok(*c),
        agg_ok(*old(cs), db),
        ddm_ok(*dm, l),
        ledger_ok(l, db, db2),
        nums_dbl(db),
        nums_dbl(db2),
    ensures
        agg_ok(*final(cs), db2),
{
    let rows = dm.rows();
    let ghost ents = dm.entries();
    proof {
        dm.model();
        lemma_ordered_unique(ents);
        assert forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is Some implies
            cls_ok(cs.at(ckey(fy as int, uom))->0, dsel(db, db2, ents, 0, ckey(fy as int, uom)), cp(fy, uom)) by {
            assert(dsel(db, db2, ents, 0, ckey(fy as int, uom)) == db);
        }
        assert forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is None implies
            zero_p(dsel(db, db2, ents, 0, ckey(fy as int, uom)), cp(fy, uom)) by {
            assert(dsel(db, db2, ents, 0, ckey(fy as int, uom)) == db);
        }
    }
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            consts_ok(*c),
            ddm_ok(*dm, l),
            ledger_ok(l, db, db2),
            nums_dbl(db),
            nums_dbl(db2),
            rows@.map_values(|row: (Key, Arc<DCls>)| (row.0.view(), *row.1)) == ents,
            forall|a: int, b: int| 0 <= a < b < ents.len() ==> (#[trigger] ents[a]).0 != (#[trigger] ents[b]).0,
            forall|pos: int| #![trigger ents[pos]] 0 <= pos < ents.len() ==> dm.at(ents[pos].0) == Some(ents[pos].1),
            agg_ok_sel(*cs, db, db2, ents, i as int),
        decreases rows.len() - i,
    {
        let key = &rows[i].0;
        let dc = &*rows[i].1;
        let fy = key.number;
        let uom = key.text.as_slice();
        let ghost p = cp(fy, uom@);
        let ghost before = *cs;
        proof {
            assert(ents[i as int] == (key.view(), *dc));
            assert(key.view() == ckey(fy as int, uom@));
            assert(dm.at(key.view()) == Some(*dc));
            assert(dsel(db, db2, ents, i as int, key.view()) == db) by {
                if exists|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == key.view() {
                    let j = choose|j: int| 0 <= j < i && (#[trigger] ents[j]).0 == key.view();
                    assert(ents[j].0 != ents[i as int].0);
                }
            }
        }
        let old = cs.get(key);
        let r = merge_class(old, dc, c, Ghost(db), Ghost(db2), Ghost(p), Ghost(l));
        match r {
            Some(cl) => { cs.put(key.share(), Arc::new(cl)); },
            None => { cs.remove(key); },
        }
        proof {
            assert forall|fy2: i64, uom2: Seq<u8>| #[trigger] cs.at(ckey(fy2 as int, uom2)) is Some implies
                cls_ok(cs.at(ckey(fy2 as int, uom2))->0, dsel(db, db2, ents, i as int + 1, ckey(fy2 as int, uom2)), cp(fy2, uom2)) by {
                if ckey(fy2 as int, uom2) == key.view() {
                    assert(dsel(db, db2, ents, i as int + 1, key.view()) == db2) by {
                        assert(exists|j: int| 0 <= j < i as int + 1 && (#[trigger] ents[j]).0 == key.view());
                    }
                    assert(fy2 == fy && uom2 == uom@);
                } else {
                    assert(cs.at(ckey(fy2 as int, uom2)) == before.at(ckey(fy2 as int, uom2)));
                    assert(dsel(db, db2, ents, i as int + 1, ckey(fy2 as int, uom2)) == dsel(db, db2, ents, i as int, ckey(fy2 as int, uom2))) by {
                        if exists|j: int| 0 <= j < i as int + 1 && (#[trigger] ents[j]).0 == ckey(fy2 as int, uom2) {
                            let j = choose|j: int| 0 <= j < i as int + 1 && (#[trigger] ents[j]).0 == ckey(fy2 as int, uom2);
                            assert(j < i);
                        }
                    }
                }
            }
            assert forall|fy2: i64, uom2: Seq<u8>| #[trigger] cs.at(ckey(fy2 as int, uom2)) is None implies
                zero_p(dsel(db, db2, ents, i as int + 1, ckey(fy2 as int, uom2)), cp(fy2, uom2)) by {
                if ckey(fy2 as int, uom2) == key.view() {
                    assert(dsel(db, db2, ents, i as int + 1, key.view()) == db2) by {
                        assert(exists|j: int| 0 <= j < i as int + 1 && (#[trigger] ents[j]).0 == key.view());
                    }
                    assert(fy2 == fy && uom2 == uom@);
                } else {
                    assert(cs.at(ckey(fy2 as int, uom2)) == before.at(ckey(fy2 as int, uom2)));
                    assert(dsel(db, db2, ents, i as int + 1, ckey(fy2 as int, uom2)) == dsel(db, db2, ents, i as int, ckey(fy2 as int, uom2))) by {
                        if exists|j: int| 0 <= j < i as int + 1 && (#[trigger] ents[j]).0 == ckey(fy2 as int, uom2) {
                            let j = choose|j: int| 0 <= j < i as int + 1 && (#[trigger] ents[j]).0 == ckey(fy2 as int, uom2);
                            assert(j < i);
                        }
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is Some implies
            cls_ok(cs.at(ckey(fy as int, uom))->0, db2, cp(fy, uom)) by {
            let k = ckey(fy as int, uom);
            if exists|j: int| 0 <= j < ents.len() && (#[trigger] ents[j]).0 == k {
                assert(dsel(db, db2, ents, ents.len() as int, k) == db2);
            } else {
                assert(dsel(db, db2, ents, ents.len() as int, k) == db);
                lemma_class_untouched(*dm, ents, l, db, db2, fy, uom);
                lemma_cls_same(cs.at(k)->0, db, db2, cp(fy, uom));
            }
        }
        assert forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is None implies
            zero_p(db2, cp(fy, uom)) by {
            let k = ckey(fy as int, uom);
            if exists|j: int| 0 <= j < ents.len() && (#[trigger] ents[j]).0 == k {
                assert(dsel(db, db2, ents, ents.len() as int, k) == db2);
            } else {
                assert(dsel(db, db2, ents, ents.len() as int, k) == db);
                lemma_class_untouched(*dm, ents, l, db, db2, fy, uom);
                assert forall|gn: Option<Seq<u8>>, gc: Option<int>| qj(db2, cp(fy, uom), true, gn, gc).n == 0 && qj(db2, cp(fy, uom), false, gn, gc).n == 0 by {
                    assert(qj(db2, cp(fy, uom), true, gn, gc) == qj(db, cp(fy, uom), true, gn, gc));
                    assert(qj(db2, cp(fy, uom), false, gn, gc) == qj(db, cp(fy, uom), false, gn, gc));
                }
            }
        }
    }
}

/// A class the batch has no contribution to has the same quantities before and after the commit.
proof fn lemma_class_untouched(dm: Index<DCls>, ents: Seq<(KeyView, DCls)>, l: Seq<Ct>, db: Db, db2: Db, fy: i64, uom: Seq<u8>)
    requires
        ddm_ok(dm, l),
        ents == dm.entries(),
        ledger_ok(l, db, db2),
        !exists|j: int| 0 <= j < ents.len() && (#[trigger] ents[j]).0 == ckey(fy as int, uom),
    ensures
        forall|inner: bool, gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] qj(db2, cp(fy, uom), inner, gn, gc) == qj(db, cp(fy, uom), inner, gn, gc),
{
    dm.model();
    let k = ckey(fy as int, uom);
    assert(dm.at(k) is None) by {
        if dm.at(k) is Some { assert(exists|j: int| 0 <= j < ents.len() && (#[trigger] ents[j]).0 == k); }
    }
    assert forall|inner: bool, gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] qj(db2, cp(fy, uom), inner, gn, gc) == qj(db, cp(fy, uom), inner, gn, gc) by {
        assert forall|x: int| 0 <= x < l.len() implies !contrib_to(inner, #[trigger] l[x], cp(fy, uom), gn, gc) by {
            assert(!ct_in(l[x], cp(fy, uom)));
        }
        lemma_dq_zero(l, cp(fy, uom), inner, gn, gc);
        lemma_ledger_q(l, db, db2, cp(fy, uom), inner, gn, gc);
    }
}

}
