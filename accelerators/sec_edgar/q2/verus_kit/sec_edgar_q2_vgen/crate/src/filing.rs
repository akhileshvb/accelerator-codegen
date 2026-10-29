// One filing's answer rows from its rescanned tables, and the cells they fill.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::keys::*;
use crate::f64bits::*;
use crate::cells::*;
use crate::group::*;
use crate::maint::*;

verus! {

/// A sub row of one filing that has a fiscal year.
pub struct SR {
    pub name: Option<Arc<Vec<u8>>>,
    pub fy: i64,
}

pub open spec fn sr_view(a: Seq<u8>, t: SR) -> SubRow {
    SubRow {
        adsh: a,
        name: match t.name { Some(n) => Some((*n)@), None => None },
        fy: Some(t.fy as int),
    }
}

pub open spec fn sr_at(idx: Index<Vec<SR>>, a: Seq<u8>) -> Seq<SR> {
    match idx.at((0int, a)) { Some(v) => v@, None => Seq::empty() }
}

pub open spec fn sr_views(a: Seq<u8>, ts: Seq<SR>) -> Seq<SubRow> {
    ts.map_values(|t: SR| sr_view(a, t))
}

pub open spec fn sr_w(idx: Index<Vec<SR>>, a: Seq<u8>, k: spec_fn(SubRow) -> int) -> int {
    sum(sr_views(a, sr_at(idx, a)), k)
}

pub open spec fn sub_prefix_w(rows: Seq<SubInput>, a: Seq<u8>, k: spec_fn(SubRow) -> int) -> int {
    sum(sub_input_seq_view(rows), |s: SubRow| if s.adsh == a && s.fy is Some { k(s) } else { 0 })
}

pub fn copy_srs(src: &Vec<SR>) -> (out: Vec<SR>)
    ensures out@ == src@,
{
    let mut out: Vec<SR> = Vec::new();
    let mut i: usize = 0;
    while i < src.len()
        invariant
            i <= src@.len(),
            out@ == src@.take(i as int),
        decreases src.len() - i,
    {
        let n = match &src[i].name { Some(x) => Some(x.clone()), None => None };
        out.push(SR { name: n, fy: src[i].fy });
        proof { assert(src@.take(i as int + 1) =~= src@.take(i as int).push(src@[i as int])); }
        i += 1;
    }
    proof { assert(src@.take(src@.len() as int) =~= src@); }
    out
}

/// The sub rows with a fiscal year, per filing.
pub fn subs_by_filing(rows: &Vec<SubInput>) -> (idx: Index<Vec<SR>>)
    ensures
        idx.wf(),
        forall|a: Seq<u8>, k: spec_fn(SubRow) -> int| #[trigger] sr_w(idx, a, k) == sub_prefix_w(rows@, a, k),
{
    let mut idx: Index<Vec<SR>> = Index::new();
    let mut i: usize = 0;
    proof {
        assert forall|a: Seq<u8>, k: spec_fn(SubRow) -> int| #[trigger] sr_w(idx, a, k) == sub_prefix_w(rows@.take(0), a, k) by {
            assert(sr_views(a, sr_at(idx, a)) =~= Seq::<SubRow>::empty());
            lemma_sum_empty(Seq::<SubRow>::empty(), k);
            assert(sub_input_seq_view(rows@.take(0)) =~= Seq::<SubRow>::empty());
            lemma_sum_empty(Seq::<SubRow>::empty(), |s: SubRow| if s.adsh == a && s.fy is Some { k(s) } else { 0 });
        }
    }
    while i < rows.len()
        invariant
            i <= rows@.len(),
            idx.wf(),
            forall|a: Seq<u8>, k: spec_fn(SubRow) -> int| #[trigger] sr_w(idx, a, k) == sub_prefix_w(rows@.take(i as int), a, k),
        decreases rows.len() - i,
    {
        let r = &rows[i];
        let ghost idx0 = idx;
        if r.fy__null == 0 {
            let akey = Arc::new(copy_vec(r.adsh.as_slice()));
            let mut cell = match idx.get(&kkey(0, &akey)) {
                Some(v) => copy_srs(&*v),
                None => Vec::new(),
            };
            let name = if r.name__null == 0 { Some(Arc::new(copy_vec(r.name.as_slice()))) } else { None };
            cell.push(SR { name, fy: r.fy });
            idx.put(kkey(0, &akey), Arc::new(cell));
        }
        proof {
            let rv = sub_input_view(*r);
            assert(rows@.take(i as int + 1) =~= rows@.take(i as int).push(*r));
            assert forall|a: Seq<u8>, k: spec_fn(SubRow) -> int| #[trigger] sr_w(idx, a, k) == sub_prefix_w(rows@.take(i as int + 1), a, k) by {
                let t0 = rows@.take(i as int);
                assert(sub_input_seq_view(rows@.take(i as int + 1)) =~= sub_input_seq_view(t0).push(rv));
                let w = |s: SubRow| if s.adsh == a && s.fy is Some { k(s) } else { 0 };
                lemma_sum_push(sub_input_seq_view(t0), rv, w);
                assert(sr_w(idx0, a, k) == sub_prefix_w(t0, a, k));
                if r.fy__null == 0 && a == r.adsh@ {
                    let before = sr_at(idx0, a);
                    let t = sr_at(idx, a)[before.len() as int];
                    assert(sr_at(idx, a) == before.push(t));
                    assert(sr_view(a, t) == rv);
                    assert(sr_views(a, sr_at(idx, a)) =~= sr_views(a, before).push(rv));
                    lemma_sum_push(sr_views(a, before), rv, k);
                } else if r.fy__null == 0 {
                    assert(sr_at(idx, a) == sr_at(idx0, a));
                } else {
                    assert(idx == idx0);
                }
            }
        }
        i += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
    idx
}


pub open spec fn mk_ent(t: MR, s: SR) -> Ent {
    Ent {
        ck: CK { fy: s.fy, uom: t.uom, d: f64_desc_key(t.val) as i64 },
        row: Row { name: s.name, tag: t.tag, val: t.val },
    }
}

pub open spec fn pairs_row(t: MR, sr: Seq<SR>) -> Seq<Ent> {
    sr.map_values(|s: SR| mk_ent(t, s))
}

pub open spec fn pairs(mr: Seq<MR>, sr: Seq<SR>) -> Seq<Ent> {
    flat(mr.map_values(|t: MR| pairs_row(t, sr)))
}

pub fn copy_ent_parts(t: &MR, s: &SR) -> (e: Ent)
    requires
        f64_finite(t.val),
    ensures
        e == mk_ent(*t, *s),
        ent_ok(e),
{
    proof { lemma_key_range(t.val); }
    let d = desc_key(t.val);
    let name = match &s.name { Some(x) => Some(x.clone()), None => None };
    Ent { ck: CK { fy: s.fy, uom: t.uom.clone(), d }, row: Row { name, tag: t.tag.clone(), val: t.val } }
}

/// Every pair of a max row and a sub row of the filing, as a cell entry.
pub fn build_ents(mr: &Vec<MR>, sr: &Vec<SR>) -> (ents: Vec<Ent>)
    requires
        forall|k: int| 0 <= k < mr@.len() ==> f64_finite((#[trigger] mr@[k]).val),
    ensures
        ents@ == pairs(mr@, sr@),
        forall|j: int| 0 <= j < ents@.len() ==> ent_ok(#[trigger] ents@[j]),
{
    let mut ents: Vec<Ent> = Vec::new();
    let mut i: usize = 0;
    proof { assert(mr@.take(0).map_values(|t: MR| pairs_row(t, sr@)) =~= Seq::<Seq<Ent>>::empty()); }
    while i < mr.len()
        invariant
            i <= mr@.len(),
            forall|k: int| 0 <= k < mr@.len() ==> f64_finite((#[trigger] mr@[k]).val),
            ents@ == pairs(mr@.take(i as int), sr@),
            forall|j: int| 0 <= j < ents@.len() ==> ent_ok(#[trigger] ents@[j]),
        decreases mr.len() - i,
    {
        let mut j: usize = 0;
        let ghost base = ents@;
        while j < sr.len()
            invariant
                j <= sr@.len(),
                i < mr@.len(),
                forall|k: int| 0 <= k < mr@.len() ==> f64_finite((#[trigger] mr@[k]).val),
                ents@ == base + pairs_row(mr@[i as int], sr@.take(j as int)),
                forall|q: int| 0 <= q < ents@.len() ==> ent_ok(#[trigger] ents@[q]),
            decreases sr.len() - j,
        {
            let e = copy_ent_parts(&mr[i], &sr[j]);
            ents.push(e);
            proof {
                assert(sr@.take(j as int + 1) =~= sr@.take(j as int).push(sr@[j as int]));
                assert(pairs_row(mr@[i as int], sr@.take(j as int + 1)) =~= pairs_row(mr@[i as int], sr@.take(j as int)).push(mk_ent(mr@[i as int], sr@[j as int])));
            }
            j += 1;
        }
        proof {
            assert(sr@.take(sr@.len() as int) =~= sr@);
            let t = mr@[i as int];
            assert(mr@.take(i as int + 1) =~= mr@.take(i as int).push(t));
            let f = |t: MR| pairs_row(t, sr@);
            assert(mr@.take(i as int + 1).map_values(f).drop_last() =~= mr@.take(i as int).map_values(f));
            assert(mr@.take(i as int + 1).map_values(f).last() == pairs_row(t, sr@));
            assert(pairs(mr@.take(i as int + 1), sr@) == pairs(mr@.take(i as int), sr@) + pairs_row(t, sr@));
        }
        i += 1;
    }
    proof { assert(mr@.take(mr@.len() as int) =~= mr@); }
    ents
}


pub open spec fn mr_sem(mr: Seq<MR>, a: Seq<u8>, db: Db) -> bool {
    forall|h: spec_fn(NumRow) -> int| #[trigger] sum(mr_views(a, mr), h)
        == sum(db.num, |n: NumRow| if is_qual(n) && n.adsh == Some(a) && is_max(db.num, n) { h(n) } else { 0 })
}

pub open spec fn sr_sem(sr: Seq<SR>, a: Seq<u8>, db: Db) -> bool {
    forall|k: spec_fn(SubRow) -> int| #[trigger] sum(sr_views(a, sr), k)
        == sum(db.sub, |s: SubRow| if s.adsh == a && s.fy is Some { k(s) } else { 0 })
}

pub proof fn lemma_pairs_sum(mr: Seq<MR>, sr: Seq<SR>, phi: spec_fn(Ent) -> int)
    ensures
        sum(pairs(mr, sr), phi) == sum(mr, |t: MR| sum(sr, |s: SR| phi(mk_ent(t, s)))),
{
    let g = |t: MR| pairs_row(t, sr);
    lemma_sum_flat_map(mr, g, phi);
    assert forall|t: MR| #[trigger] sum(g(t), phi) == sum(sr, |s: SR| phi(mk_ent(t, s))) by {
        lemma_sum_map(sr, |s: SR| mk_ent(t, s), phi, |s: SR| phi(mk_ent(t, s)));
    }
    lemma_sum_ext_all(mr, |t: MR| sum(g(t), phi), |t: MR| sum(sr, |s: SR| phi(mk_ent(t, s))));
}

/// The entries of a filing weigh what the database says its cell does.
pub proof fn lemma_ents_cell_w(mr: Seq<MR>, sr: Seq<SR>, a: Seq<u8>, db: Db, id: CellId, g: spec_fn(OutRow) -> int)
    requires
        mr_sem(mr, a, db),
        sr_sem(sr, a, db),
        forall|k: int| 0 <= k < mr.len() ==> f64_finite((#[trigger] mr[k]).val),
        id.a == a,
    ensures
        ent_sum(pairs(mr, sr), id, g) == cell_w(db, id, g),
{
    let phi = |e: Ent| if ck_matches(e.ck, id) { g(e.row.view()) } else { 0 };
    lemma_pairs_sum(mr, sr, phi);
    let hh = |n: NumRow| if n.uom == Some(id.uom) && vkey(n.value->0, id.d) {
        sum(db.sub, |s: SubRow| if s.adsh == a && s.fy == Some(id.fy) { g(ent_row(n, s)) } else { 0 })
    } else { 0 };
    let sel = |n: NumRow| if is_qual(n) && n.adsh == Some(a) && is_max(db.num, n) { hh(n) } else { 0 };
    let cwt = |n: NumRow| if picked(db.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) { pair_w(db.sub, n, id.fy, g) } else { 0 };
    assert forall|n: NumRow| #[trigger] cwt(n) == sel(n) by {
        if picked(db.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) {
            let l = |s: SubRow| if sub_hits(n, s, id.fy) { g(ent_row(n, s)) } else { 0 };
            let r = |s: SubRow| if s.adsh == a && s.fy == Some(id.fy) { g(ent_row(n, s)) } else { 0 };
            assert forall|s: SubRow| #[trigger] l(s) == r(s) by {}
            lemma_sum_ext_all(db.sub, l, r);
        }
    }
    lemma_sum_ext_all(db.num, cwt, sel);
    assert(sum(mr_views(a, mr), hh) == sum(db.num, sel));
    lemma_sum_map(mr, |t: MR| mr_view(a, t), hh, |t: MR| hh(mr_view(a, t)));
    let inner = |t: MR| sum(sr, |s: SR| phi(mk_ent(t, s)));
    assert forall|t: MR| mr.contains(t) implies #[trigger] inner(t) == hh(mr_view(a, t)) by {
        let k = choose|k: int| 0 <= k < mr.len() && mr[k] == t;
        assert(f64_finite(t.val));
        let nv = mr_view(a, t);
        lemma_vkey_iff(t.val, id.d);
        lemma_key_range(t.val);
        let kk = |s: SubRow| if s.fy == Some(id.fy) { g(ent_row(nv, s)) } else { 0 };
        let r2 = |s: SubRow| if s.adsh == a && s.fy is Some { kk(s) } else { 0 };
        let r3 = |s: SubRow| if s.adsh == a && s.fy == Some(id.fy) { g(ent_row(nv, s)) } else { 0 };
        assert forall|s: SubRow| #[trigger] r2(s) == r3(s) by {}
        lemma_sum_ext_all(db.sub, r2, r3);
        assert(sum(sr_views(a, sr), kk) == sum(db.sub, r2));
        let step = |s: SR| phi(mk_ent(t, s));
        let kv = |s: SR| kk(sr_view(a, s));
        lemma_sum_map(sr, |s: SR| sr_view(a, s), kk, kv);
        if (*t.uom)@ == id.uom && f64_desc_key(t.val) == id.d {
            assert forall|s: SR| #[trigger] step(s) == kv(s) by {
                assert(ent_row(nv, sr_view(a, s)) == mk_ent(t, s).row.view());
            }
            lemma_sum_ext_all(sr, step, kv);
        } else {
            let z = |s: SR| 0int;
            assert forall|s: SR| #[trigger] step(s) == z(s) by {}
            lemma_sum_ext_all(sr, step, z);
            lemma_sum_zero(sr, z);
        }
    }
    assert forall|i: int| 0 <= i < mr.len() implies inner(#[trigger] mr[i]) == hh(mr_view(a, mr[i])) by {
        assert(mr.contains(mr[i]));
    }
    lemma_sum_ext(mr, inner, |t: MR| hh(mr_view(a, t)));
}

} // verus!
