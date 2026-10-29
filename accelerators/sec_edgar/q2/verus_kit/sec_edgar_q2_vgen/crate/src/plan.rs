// What a batch of changed filings asks the host to rescan: the filings themselves, and, in a class whose
// stored prefix they may have shortened, the next filings in line, so the prefix keeps its hundred cells.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_common::order::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows, lower_rows, lower_position, lemma_lower_suffix, lemma_key_order};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::cells::*;
use crate::classes::*;
use crate::maint::*;
use crate::group::copy_vec;
use crate::thr::*;
use crate::cnt::*;
use verus_prooflib::ordered::lemma_ordered_unique;

verus! {

/// Filings taken from the next in line, per class (ties at the last included).
pub const NPICK: usize = 512;

pub open spec fn npick() -> int { NPICK as int }

/// Filing a has a stored cell of class (fy, uom) within its threshold.
pub open spec fn hit_by(dir: Index<Vec<CK>>, thr: Thr, a: Seq<u8>, fy: int, uom: Seq<u8>) -> bool {
    thr_of(thr, fy, uom) < D_INF as int && exists|j: int| 0 <= j < dir_of(dir, a).len()
        && (#[trigger] dir_of(dir, a)[j]).fy as int == fy && (*dir_of(dir, a)[j].uom)@ == uom && dir_of(dir, a)[j].d as int <= thr_of(thr, fy, uom)
}

/// Some changed filing has a stored cell of the class within its threshold.
pub open spec fn cls_hit(dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>, fy: int, uom: Seq<u8>) -> bool {
    exists|i: int| 0 <= i < tk.len() && hit_by(dir, thr, #[trigger] tk[i], fy, uom)
}

/// The class's entries beyond its threshold, except those of changed filings.
pub open spec fn tail_in(es: Seq<(KeyView, Vec<Row>)>, d: int, tk: Seq<Seq<u8>>) -> Seq<(KeyView, Vec<Row>)> {
    es.filter(|e: (KeyView, Vec<Row>)| e.0.0 > d && !tk.contains(e.0.1))
}

pub open spec fn tail_of(cells: Cells, thr: Thr, tk: Seq<Seq<u8>>, fy: int, uom: Seq<u8>) -> Seq<(KeyView, Vec<Row>)> {
    tail_in(class_entries(cells, fy, uom), thr_of(thr, fy, uom), tk)
}

pub open spec fn pivot_of(tail: Seq<(KeyView, Vec<Row>)>) -> int {
    if tail.len() <= npick() { D_INF as int } else { tail[npick() - 1].0.0 }
}

pub open spec fn picked_of(tail: Seq<(KeyView, Vec<Row>)>) -> Seq<Seq<u8>> {
    tail.filter(|e: (KeyView, Vec<Row>)| e.0.0 <= pivot_of(tail)).map_values(|e: (KeyView, Vec<Row>)| e.0.1)
}

/// The class's threshold after the batch.
pub open spec fn dnew(cells: Cells, dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>, fy: int, uom: Seq<u8>) -> int {
    if cls_hit(dir, thr, tk, fy, uom) { pivot_of(tail_of(cells, thr, tk, fy, uom)) } else { thr_of(thr, fy, uom) }
}

pub open spec fn wanted_has(cells: Cells, dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>, a: Seq<u8>) -> bool {
    tk.contains(a) || exists|fy: int, uom: Seq<u8>| cls_hit(dir, thr, tk, fy, uom) && picked_of(#[trigger] tail_of(cells, thr, tk, fy, uom)).contains(a)
}

pub open spec fn bytes_order() -> spec_fn(Seq<u8>, Seq<u8>) -> bool {
    |a: Seq<u8>, b: Seq<u8>| bytes_lt(a, b)
}

pub open spec fn is_wanted(cells: Cells, dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>, q: Seq<Seq<u8>>) -> bool {
    &&& sorted_by(q, bytes_order())
    &&& forall|a: Seq<u8>| q.contains(a) <==> wanted_has(cells, dir, thr, tk, a)
}

/// The keys to rescan: the changed filings and the next in line, each once, in byte order.
pub open spec fn wanted_keys(cells: Cells, dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>) -> Seq<Seq<u8>> {
    choose|q: Seq<Seq<u8>>| is_wanted(cells, dir, thr, tk, q)
}

pub proof fn lemma_bytes_strict()
    ensures
        strict_order(bytes_order()),
{
    assert forall|x: Seq<u8>| !#[trigger] bytes_order()(x, x) by { lemma_bytes_lt_irrefl(x); }
    assert forall|x: Seq<u8>, y: Seq<u8>, z: Seq<u8>| #[trigger] bytes_order()(x, y) && #[trigger] bytes_order()(y, z) implies bytes_order()(x, z) by {
        lemma_bytes_lt_trans(x, y, z);
    }
}

/// A sorted list with the wanted members is the wanted list.
pub proof fn lemma_wanted_unique(cells: Cells, dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>, q: Seq<Seq<u8>>)
    requires
        is_wanted(cells, dir, thr, tk, q),
    ensures
        wanted_keys(cells, dir, thr, tk) == q,
{
    lemma_bytes_strict();
    let w = wanted_keys(cells, dir, thr, tk);
    assert(is_wanted(cells, dir, thr, tk, w));
    lemma_sorted_unique(w, q, bytes_order());
}

// ---- the changed filings as an index ----

pub fn units_index(tk: &Vec<Vec<u8>>) -> (ix: Index<bool>)
    ensures
        ix.wf(),
        forall|a: Seq<u8>| #[trigger] ix.at((0int, a)) is Some <==> seq_view_bytes(tk@).contains(a),
        forall|k: KeyView| #[trigger] ix.at(k) is Some ==> k.0 == 0,
{
    let mut ix: Index<bool> = Index::new();
    let mut i: usize = 0;
    let ghost tv = seq_view_bytes(tk@);
    while i < tk.len()
        invariant
            i <= tk@.len(),
            tv == seq_view_bytes(tk@),
            ix.wf(),
            forall|a: Seq<u8>| #[trigger] ix.at((0int, a)) is Some <==> tv.take(i as int).contains(a),
            forall|k: KeyView| #[trigger] ix.at(k) is Some ==> k.0 == 0,
        decreases tk.len() - i,
    {
        ix.put(Key::composite(0, copy_vec(tk[i].as_slice())), Arc::new(true));
        proof {
            assert(tv.take(i as int + 1) =~= tv.take(i as int).push(tv[i as int]));
            assert forall|a: Seq<u8>| #[trigger] ix.at((0int, a)) is Some <==> tv.take(i as int + 1).contains(a) by {
                crate::filing::lemma_push_contains2(tv.take(i as int), tv[i as int], a);
            }
            assert(tv[i as int] == tk@[i as int]@);
        }
        i += 1;
    }
    proof { assert(tv.take(tv.len() as int) =~= tv); }
    ix
}

// ---- the classes the batch may shorten ----

pub fn hit_classes(dir: &Index<Vec<CK>>, thr: &Thr, tk: &Vec<Vec<u8>>) -> (hits: Index<bool>)
    requires
        dir.wf(),
        thr.wf(),
    ensures
        hits.wf(),
        forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> cls_hit(*dir, *thr, seq_view_bytes(tk@), fy, uom),
{
    let mut hits: Index<bool> = Index::new();
    let tv = Ghost(seq_view_bytes(tk@));
    let mut i: usize = 0;
    while i < tk.len()
        invariant
            i <= tk@.len(),
            tv@ == seq_view_bytes(tk@),
            hits.wf(),
            dir.wf(),
            thr.wf(),
            forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> cls_hit(*dir, *thr, tv@.take(i as int), fy, uom),
        decreases tk.len() - i,
    {
        let key = Key::composite(0, copy_vec(tk[i].as_slice()));
        let ghost a = tk@[i as int]@;
        let ghost before = hits;
        match dir.get(&key) {
            None => {
                proof {
                    assert(dir_of(*dir, a) == Seq::<CK>::empty());
                    assert forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> cls_hit(*dir, *thr, tv@.take(i as int + 1), fy, uom) by {
                        assert(!hit_by(*dir, *thr, a, fy, uom));
                        assert(tv@.take(i as int + 1) =~= tv@.take(i as int).push(tv@[i as int]));
                        if cls_hit(*dir, *thr, tv@.take(i as int + 1), fy, uom) {
                            let q = choose|q: int| 0 <= q < tv@.take(i as int + 1).len() && hit_by(*dir, *thr, #[trigger] tv@.take(i as int + 1)[q], fy, uom);
                            if q < i as int { assert(tv@.take(i as int)[q] == tv@.take(i as int + 1)[q]); }
                        }
                        if cls_hit(*dir, *thr, tv@.take(i as int), fy, uom) {
                            let q = choose|q: int| 0 <= q < tv@.take(i as int).len() && hit_by(*dir, *thr, #[trigger] tv@.take(i as int)[q], fy, uom);
                            assert(tv@.take(i as int + 1)[q] == tv@.take(i as int)[q]);
                        }
                    }
                }
            }
            Some(l) => {
                let mut j: usize = 0;
                proof { assert(dir_of(*dir, a) == l@); }
                while j < l.len()
                    invariant
                        j <= l@.len(),
                        l@ == dir_of(*dir, a),
                        a == tv@[i as int],
                        i < tv@.len(),
                        hits.wf(),
                        thr.wf(),
                        forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> (cls_hit(*dir, *thr, tv@.take(i as int), fy, uom)
                            || (thr_of(*thr, fy, uom) < D_INF as int && exists|q: int| 0 <= q < j && (#[trigger] l@[q]).fy as int == fy && (*l@[q].uom)@ == uom && l@[q].d as int <= thr_of(*thr, fy, uom))),
                    decreases l.len() - j,
                {
                    let dt = thr_get(thr, l[j].fy, &l[j].uom);
                    if dt < D_INF && l[j].d <= dt {
                        hits.put(kkey(l[j].fy, &l[j].uom), Arc::new(true));
                    }
                    proof {
                        assert forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> (cls_hit(*dir, *thr, tv@.take(i as int), fy, uom)
                            || (thr_of(*thr, fy, uom) < D_INF as int && exists|q: int| 0 <= q < j + 1 && (#[trigger] l@[q]).fy as int == fy && (*l@[q].uom)@ == uom && l@[q].d as int <= thr_of(*thr, fy, uom))) by {
                            if (fy, uom) == (l[j as int].fy as int, (*l[j as int].uom)@) {
                                if dt < D_INF && l[j as int].d <= dt {
                                    assert(l@[j as int].fy as int == fy);
                                }
                            }
                            if thr_of(*thr, fy, uom) < D_INF as int && exists|q: int| 0 <= q < j + 1 && (#[trigger] l@[q]).fy as int == fy && (*l@[q].uom)@ == uom && l@[q].d as int <= thr_of(*thr, fy, uom) {
                                let q = choose|q: int| 0 <= q < j + 1 && (#[trigger] l@[q]).fy as int == fy && (*l@[q].uom)@ == uom && l@[q].d as int <= thr_of(*thr, fy, uom);
                                if q == j as int {
                                    assert(dt as int == thr_of(*thr, fy, uom));
                                }
                            }
                        }
                    }
                    j += 1;
                }
                proof {
                    assert(tv@.take(i as int + 1) =~= tv@.take(i as int).push(tv@[i as int]));
                    assert forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> cls_hit(*dir, *thr, tv@.take(i as int + 1), fy, uom) by {
                        let t1 = tv@.take(i as int + 1);
                        if cls_hit(*dir, *thr, t1, fy, uom) {
                            let q = choose|q: int| 0 <= q < t1.len() && hit_by(*dir, *thr, #[trigger] t1[q], fy, uom);
                            if q < i as int { assert(tv@.take(i as int)[q] == t1[q]); }
                            else { assert(t1[q] == a); }
                        }
                        if cls_hit(*dir, *thr, tv@.take(i as int), fy, uom) {
                            let q = choose|q: int| 0 <= q < tv@.take(i as int).len() && hit_by(*dir, *thr, #[trigger] tv@.take(i as int)[q], fy, uom);
                            assert(t1[q] == tv@.take(i as int)[q]);
                        }
                        if thr_of(*thr, fy, uom) < D_INF as int && exists|q: int| 0 <= q < l@.len() && (#[trigger] l@[q]).fy as int == fy && (*l@[q].uom)@ == uom && l@[q].d as int <= thr_of(*thr, fy, uom) {
                            assert(hit_by(*dir, *thr, a, fy, uom));
                            assert(t1[i as int] == a);
                        }
                    }
                }
            }
        }
        i += 1;
    }
    proof { assert(tv@.take(tv@.len() as int) =~= tv@); }
    hits
}

// ---- filters ----

pub proof fn lemma_filter_concat<A>(a: Seq<A>, b: Seq<A>, p: spec_fn(A) -> bool)
    ensures
        (a + b).filter(p) == a.filter(p) + b.filter(p),
    decreases b.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if b.len() == 0 {
        assert(a + b =~= a);
        assert(b.filter(p) == Seq::<A>::empty());
        assert(a.filter(p) + b.filter(p) =~= a.filter(p));
    } else {
        lemma_filter_concat(a, b.drop_last(), p);
        assert((a + b).drop_last() =~= a + b.drop_last());
        assert((a + b).last() == b.last());
        if p(b.last()) {
            assert(a.filter(p) + b.filter(p) =~= (a.filter(p) + b.drop_last().filter(p)).push(b.last()));
        } else {
            assert(a.filter(p) + b.filter(p) =~= a.filter(p) + b.drop_last().filter(p));
        }
    }
}

pub proof fn lemma_filter_none<A>(s: Seq<A>, p: spec_fn(A) -> bool)
    requires
        forall|i: int| 0 <= i < s.len() ==> !p(#[trigger] s[i]),
    ensures
        s.filter(p) == Seq::<A>::empty(),
    decreases s.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if s.len() > 0 {
        lemma_filter_none(s.drop_last(), p);
        assert(!p(s.last()));
    }
}

pub proof fn lemma_filter_all<A>(s: Seq<A>, p: spec_fn(A) -> bool)
    requires
        forall|i: int| 0 <= i < s.len() ==> p(#[trigger] s[i]),
    ensures
        s.filter(p) == s,
    decreases s.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if s.len() > 0 {
        lemma_filter_all(s.drop_last(), p);
        assert(s.drop_last().filter(p) == s.drop_last());
        assert(s.drop_last().push(s.last()) =~= s);
    }
}

/// Filtering past a prefix the predicate rejects.
pub proof fn lemma_filter_skip<A>(s: Seq<A>, lp: int, p: spec_fn(A) -> bool)
    requires
        0 <= lp <= s.len(),
        forall|i: int| 0 <= i < lp ==> !p(#[trigger] s[i]),
    ensures
        s.filter(p) == s.skip(lp).filter(p),
{
    assert(s =~= s.take(lp) + s.skip(lp));
    lemma_filter_concat(s.take(lp), s.skip(lp), p);
    assert forall|i: int| 0 <= i < s.take(lp).len() implies !p(#[trigger] s.take(lp)[i]) by {
        assert(s.take(lp)[i] == s[i]);
    }
    lemma_filter_none(s.take(lp), p);
    assert(Seq::<A>::empty() + s.skip(lp).filter(p) =~= s.skip(lp).filter(p));
}

pub proof fn lemma_filter_step<A>(s: Seq<A>, j: int, p: spec_fn(A) -> bool)
    requires
        0 <= j < s.len(),
    ensures
        s.take(j + 1).filter(p) == if p(s[j]) { s.take(j).filter(p).push(s[j]) } else { s.take(j).filter(p) },
{
    reveal_with_fuel(Seq::filter, 2);
    let t = s.take(j + 1);
    assert(t.drop_last() =~= s.take(j));
    assert(t.last() == s[j]);
}

pub proof fn lemma_filter_ext<A>(s: Seq<A>, p: spec_fn(A) -> bool, q: spec_fn(A) -> bool)
    requires
        forall|i: int| 0 <= i < s.len() ==> p(#[trigger] s[i]) == q(s[i]),
    ensures
        s.filter(p) == s.filter(q),
    decreases s.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if s.len() > 0 {
        lemma_filter_ext(s.drop_last(), p, q);
        assert forall|i: int| 0 <= i < s.drop_last().len() implies p(#[trigger] s.drop_last()[i]) == q(s.drop_last()[i]) by {
            assert(s.drop_last()[i] == s[i]);
        }
        assert(p(s.last()) == q(s.last()));
    }
}

/// Entries of an ordered list have value keys that do not decrease.
pub proof fn lemma_ordered_d(tl: Seq<(KeyView, Vec<Row>)>)
    requires
        ordered_rows(tl),
    ensures
        forall|p: int, q: int| 0 <= p < q < tl.len() ==> (#[trigger] tl[p]).0.0 <= (#[trigger] tl[q]).0.0,
{
    assert forall|p: int, q: int| 0 <= p < q < tl.len() implies (#[trigger] tl[p]).0.0 <= (#[trigger] tl[q]).0.0 by {
        assert(key_lt(tl[p].0, tl[q].0));
    }
}

pub open spec fn pick_view(v: Seq<Arc<Vec<u8>>>) -> Seq<Seq<u8>> {
    v.map_values(|a: Arc<Vec<u8>>| (*a)@)
}

pub open spec fn units_of(es: Seq<(KeyView, Vec<Row>)>) -> Seq<Seq<u8>> {
    es.map_values(|e: (KeyView, Vec<Row>)| e.0.1)
}

/// An element of a filtered prefix is an element of the list at an earlier position.
pub proof fn lemma_member_pos<A>(s: Seq<A>, j: int, p: spec_fn(A) -> bool, i: int) -> (pos: int)
    requires
        0 <= j <= s.len(),
        0 <= i < s.take(j).filter(p).len(),
    ensures
        0 <= pos < j,
        s[pos] == s.take(j).filter(p)[i],
{
    let x = s.take(j).filter(p)[i];
    assert(s.take(j).filter(p).contains(x));
    s.take(j).lemma_filter_contains_rev(p, x);
    let pos = choose|pos: int| 0 <= pos < s.take(j).len() && s.take(j)[pos] == x;
    assert(s.take(j)[pos] == s[pos]);
    pos
}

/// The first non-changed filings of a class beyond its threshold d, closed at the last one's value key.
pub fn scan_class(inner: &Index<Vec<Row>>, d: i64, tset: &Index<bool>, Ghost(tk): Ghost<Seq<Seq<u8>>>) -> (res: (Vec<Arc<Vec<u8>>>, i64))
    requires
        inner.wf(),
        d < D_INF,
        forall|a: Seq<u8>| #[trigger] tset.at((0int, a)) is Some <==> tk.contains(a),
        forall|i: int| 0 <= i < inner.entries().len() ==> (#[trigger] inner.entries()[i]).0.0 <= D_INF as int,
    ensures
        ({
            let tail = tail_in(inner.entries(), d as int, tk);
            &&& pick_view(res.0@) == picked_of(tail)
            &&& res.1 as int == pivot_of(tail)
        }),
{
    let ghost es = inner.entries();
    proof { inner.model(); lemma_lead(es, d as int); lemma_lower_suffix(es, bound_of(d as int)); }
    let ghost lp = lead(es, d as int);
    let ghost tl = es.skip(lp);
    let ghost nonp = |e: (KeyView, Vec<Row>)| !tk.contains(e.0.1);
    let ghost beyond = |e: (KeyView, Vec<Row>)| e.0.0 > d as int && !tk.contains(e.0.1);
    proof {
        assert forall|i: int| 0 <= i < lp implies !beyond(#[trigger] es[i]) by {}
        lemma_filter_skip(es, lp, beyond);
        assert forall|i: int| 0 <= i < tl.len() implies (beyond(#[trigger] tl[i]) == nonp(tl[i])) by {
            assert(tl[i] == es[lp + i]);
        }
        lemma_filter_ext(tl, beyond, nonp);
        assert(tail_in(es, d as int, tk) == tl.filter(nonp));
        lemma_ordered_d(tl);
        assert forall|i: int| 0 <= i < tl.len() implies (#[trigger] tl[i]).0.0 <= D_INF as int by { assert(tl[i] == es[lp + i]); }
    }
    let bkey = Key::number(d + 1);
    let mut cur = inner.cursor_from(&bkey);
    proof {
        assert(bkey.view() == bound_of(d as int));
        assert(cur.remaining() == lower_rows(es, bound_of(d as int)));
        assert(cur.remaining() == tl);
    }
    let mut picks: Vec<Arc<Vec<u8>>> = Vec::new();
    let mut piv: i64 = D_INF;
    let mut stopped = false;
    let ghost mut j: int = 0;
    proof { assert(tl.take(0) =~= Seq::<(KeyView, Vec<Row>)>::empty()); assert(tl.skip(0) =~= tl); }
    loop
        invariant_except_break
            !stopped,
            cur.remaining() == tl.skip(j),
        invariant
            inner.wf(),
            es == inner.entries(),
            tl == es.skip(lp),
            0 <= lp <= es.len(),
            ordered_rows(tl),
            forall|p: int, q: int| 0 <= p < q < tl.len() ==> (#[trigger] tl[p]).0.0 <= (#[trigger] tl[q]).0.0,
            forall|i: int| 0 <= i < tl.len() ==> (#[trigger] tl[i]).0.0 <= D_INF as int,
            forall|a: Seq<u8>| #[trigger] tset.at((0int, a)) is Some <==> tk.contains(a),
            0 <= j <= tl.len(),
            cur.work() >= 0,
            pick_view(picks@) == units_of(tl.take(j).filter(nonp)),
            picks@.len() == tl.take(j).filter(nonp).len(),
            picks@.len() >= NPICK ==> piv as int == tl.take(j).filter(nonp)[npick() - 1].0.0,
            picks@.len() >= NPICK ==> forall|i: int| 0 <= i < picks@.len() ==> (#[trigger] tl.take(j).filter(nonp)[i]).0.0 <= piv as int,
            nonp == (|e: (KeyView, Vec<Row>)| !tk.contains(e.0.1)),
        ensures
            0 <= j <= tl.len(),
            stopped ==> j < tl.len() && nonp(tl[j]) && picks@.len() >= NPICK && tl[j].0.0 > piv as int,
            !stopped ==> j == tl.len(),
            pick_view(picks@) == units_of(tl.take(j).filter(nonp)),
            picks@.len() == tl.take(j).filter(nonp).len(),
            picks@.len() >= NPICK ==> piv as int == tl.take(j).filter(nonp)[npick() - 1].0.0,
            picks@.len() >= NPICK ==> forall|i: int| 0 <= i < picks@.len() ==> (#[trigger] tl.take(j).filter(nonp)[i]).0.0 <= piv as int,
        decreases cur.work(),
    {
        let ghost before = cur.remaining();
        match cur.next() {
            None => {
                proof { assert(tl.skip(j).len() == 0); }
                break;
            }
            Some((k, cell)) => {
                proof {
                    assert(before.len() > 0);
                    assert(before[0] == tl[j]);
                    assert(cur.remaining() == tl.skip(j).skip(1));
                    assert(tl.skip(j).skip(1) =~= tl.skip(j + 1));
                    lemma_filter_step(tl, j, nonp);
                    assert((k.view(), *cell) == tl[j]);
                }
                let fresh = tset.get(&Key::shared(0, k.text.clone())).is_none();
                proof { assert(fresh == nonp(tl[j])); }
                if fresh {
                    if picks.len() >= NPICK && k.number > piv {
                        stopped = true;
                        break;
                    }
                    let ghost old = tl.take(j).filter(nonp);
                    if picks.len() == NPICK - 1 {
                        piv = k.number;
                    }
                    picks.push(k.text.clone());
                    proof {
                        let nt = tl.take(j + 1).filter(nonp);
                        assert(nt == old.push(tl[j]));
                        assert(pick_view(picks@) =~= pick_view(picks@.drop_last()).push((*k.text)@));
                        assert(units_of(nt) =~= units_of(old).push(tl[j].0.1));
                        assert(tl[j].0.1 == (*k.text)@);
                        assert forall|i: int| 0 <= i < old.len() implies (#[trigger] old[i]).0.0 <= tl[j].0.0 by {
                            let pos = lemma_member_pos(tl, j, nonp, i);
                        }
                        if old.len() >= npick() {
                            assert(k.number as int <= piv as int);
                        }
                        if nt.len() >= npick() {
                            assert forall|i: int| 0 <= i < nt.len() implies (#[trigger] nt[i]).0.0 <= piv as int by {
                                if i < old.len() {
                                    assert(nt[i] == old[i]);
                                } else {
                                    assert(nt[i] == tl[j]);
                                }
                            }
                            if old.len() == npick() - 1 {
                                assert(nt[npick() - 1] == tl[j]);
                            } else {
                                assert(nt[npick() - 1] == old[npick() - 1]);
                            }
                        }
                    }
                } else {
                    proof { assert(tl.take(j + 1).filter(nonp) == tl.take(j).filter(nonp)); }
                }
                proof { j = j + 1; }
            }
        }
    }
    proof {
        let nt = tl.filter(nonp);
        let pfx = tl.take(j).filter(nonp);
        let sfx = tl.skip(j).filter(nonp);
        assert(tl =~= tl.take(j) + tl.skip(j));
        lemma_filter_concat(tl.take(j), tl.skip(j), nonp);
        assert(nt == pfx + sfx);
        let tail = tail_in(es, d as int, tk);
        assert(tail == nt);
        if !stopped {
            assert(j == tl.len());
            assert(tl.take(j) =~= tl);
            assert(pfx == nt);
            assert forall|i: int| 0 <= i < nt.len() implies (#[trigger] nt[i]).0.0 <= pivot_of(nt) by {
                if nt.len() <= npick() {
                    let pos = lemma_member_pos(tl, j, nonp, i);
                    assert(tl[pos] == nt[i]);
                } else {
                    assert(picks@.len() >= NPICK);
                }
            }
            lemma_filter_all(nt, |e: (KeyView, Vec<Row>)| e.0.0 <= pivot_of(nt));
            assert(picked_of(nt) == units_of(nt));
        } else {
            assert(tl.skip(j).contains(tl[j]));
            assert(nonp(tl[j]));
            assert(sfx.len() >= 1) by {
                tl.skip(j).lemma_filter_contains(nonp, 0);
                assert(tl.skip(j)[0] == tl[j]);
            }
            assert(nt.len() > npick());
            assert(nt[npick() - 1] == pfx[npick() - 1]);
            assert(pivot_of(nt) == piv as int);
            assert forall|i: int| 0 <= i < sfx.len() implies !((#[trigger] sfx[i]).0.0 <= piv as int) by {
                let x = sfx[i];
                assert(sfx.contains(x));
                tl.skip(j).lemma_filter_contains_rev(nonp, x);
                let pos = choose|pos: int| 0 <= pos < tl.skip(j).len() && tl.skip(j)[pos] == x;
                assert(tl.skip(j)[pos] == tl[j + pos]);
                if pos > 0 { assert(tl[j].0.0 <= tl[j + pos].0.0); }
            }
            let le_p = |e: (KeyView, Vec<Row>)| e.0.0 <= pivot_of(nt);
            lemma_filter_concat(pfx, sfx, le_p);
            lemma_filter_none(sfx, le_p);
            lemma_filter_all(pfx, le_p);
            assert(nt.filter(le_p) == pfx);
            assert(picked_of(nt) == units_of(pfx));
        }
    }
    let res_piv = if stopped || picks.len() > NPICK { piv } else { D_INF };
    (picks, res_piv)
}

/// Every cell key's value key fits a 64-bit integer.
pub open spec fn keys_in_range(cells: Cells) -> bool {
    forall|fy: int, uom: Seq<u8>, i: int| 0 <= i < class_entries(cells, fy, uom).len() ==> (#[trigger] class_entries(cells, fy, uom)[i]).0.0 <= D_INF as int
}

pub open spec fn cells_wf(cells: Cells) -> bool {
    &&& cells.wf()
    &&& forall|k: KeyView| #[trigger] cells.at(k) is Some ==> cells.at(k)->0.wf()
}

/// The keys to rescan and the thresholds after the batch.
pub fn plan(cells: &Cells, dir: &Index<Vec<CK>>, thr: &Thr, tk: &Vec<Vec<u8>>) -> (r: (Vec<Vec<u8>>, Thr))
    requires
        cells_wf(*cells),
        keys_in_range(*cells),
        dir.wf(),
        thr.wf(),
    ensures
        is_wanted(*cells, *dir, *thr, seq_view_bytes(tk@), seq_view_bytes(r.0@)),
        r.1.wf(),
        forall|fy: int, uom: Seq<u8>| thr_of(r.1, fy, uom) == dnew(*cells, *dir, *thr, seq_view_bytes(tk@), fy, uom),
{
    let ghost tkv = seq_view_bytes(tk@);
    let tset = units_index(tk);
    let hits = hit_classes(dir, thr, tk);
    let mut out = tset.share();
    let mut thr_new = thr.share();
    let hrows = hits.rows();
    proof { hits.map_model(); }
    let ghost hk = hrows@.map_values(|row: (Key, Arc<bool>)| row.0.view());
    proof {
        assert forall|k: KeyView| out.at(k) == tset.at(k) by {}
        assert(hk.take(0) =~= Seq::<KeyView>::empty());
        assert forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (tkv.contains(a)
            || exists|fy: int, uom: Seq<u8>| hk.take(0).contains((fy, uom)) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy, uom)).contains(a)) by {
            assert(tset.at((0int, a)) is Some <==> tkv.contains(a));
        }
        assert forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thr_new, fy, uom) == if hk.take(0).contains((fy, uom)) { dnew(*cells, *dir, *thr, tkv, fy, uom) } else { thr_of(*thr, fy, uom) } by {}
        assert(hrows@.map_values(|row: (Key, Arc<bool>)| (row.0.view(), *row.1)) == hits.entries());
        lemma_ordered_unique(hits.entries());
    }
    let mut i: usize = 0;
    while i < hrows.len()
        invariant
            i <= hrows@.len(),
            cells_wf(*cells),
            keys_in_range(*cells),
            dir.wf(),
            thr.wf(),
            hits.wf(),
            tset.wf(),
            out.wf(),
            thr_new.wf(),
            hrows@.map_values(|row: (Key, Arc<bool>)| (row.0.view(), *row.1)) == hits.entries(),
            hk == hrows@.map_values(|row: (Key, Arc<bool>)| row.0.view()),
            forall|a: Seq<u8>| #[trigger] tset.at((0int, a)) is Some <==> tkv.contains(a),
            forall|fy: int, uom: Seq<u8>| #[trigger] hits.at((fy, uom)) is Some <==> cls_hit(*dir, *thr, tkv, fy, uom),
            forall|k: KeyView| #[trigger] out.at(k) is Some ==> k.0 == 0,
            forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thr_new, fy, uom) == if hk.take(i as int).contains((fy, uom)) { dnew(*cells, *dir, *thr, tkv, fy, uom) } else { thr_of(*thr, fy, uom) },
            forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (tkv.contains(a)
                || exists|fy: int, uom: Seq<u8>| hk.take(i as int).contains((fy, uom)) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy, uom)).contains(a)),
        decreases hrows.len() - i,
    {
        let ghost prev_out = out;
        let ghost prev_thr = thr_new;
        let ckey = hrows[i].0.share();
        let fy = ckey.number;
        let uom = ckey.text.clone();
        proof {
            hits.map_model();
            assert(hits.entries()[i as int] == (hrows@[i as int].0.view(), *hrows@[i as int].1));
            assert(hits.entries()[i as int].0 == ckey.view());
            assert(hits.entries().contains(hits.entries()[i as int]));
            assert(hits.view().dom().contains(ckey.view()));
            assert(hits.at(ckey.view()) is Some);
            assert(cls_hit(*dir, *thr, tkv, fy as int, (*uom)@));
            assert(hk[i as int] == ckey.view());
            assert(ckey.view() == (fy as int, (*uom)@));
        }
        let dcur = thr_get(thr, fy, &uom);
        proof {
            let ii = choose|ii: int| 0 <= ii < tkv.len() && hit_by(*dir, *thr, #[trigger] tkv[ii], fy as int, (*uom)@);
            assert(hit_by(*dir, *thr, tkv[ii], fy as int, (*uom)@));
            assert(dcur < D_INF);
        }
        match cells.get(&ckey) {
            None => {
                proof {
                    assert(class_entries(*cells, fy as int, (*uom)@) =~= Seq::<(KeyView, Vec<Row>)>::empty());
                    assert(tail_of(*cells, *thr, tkv, fy as int, (*uom)@) =~= Seq::<(KeyView, Vec<Row>)>::empty());
                    assert(tail_of(*cells, *thr, tkv, fy as int, (*uom)@).filter(|e: (KeyView, Vec<Row>)| e.0.0 <= pivot_of(tail_of(*cells, *thr, tkv, fy as int, (*uom)@))).len() == 0);
                }
                thr_new.remove(&ckey);
                proof {
                    let tl0 = tail_of(*cells, *thr, tkv, fy as int, (*uom)@);
                    assert(pivot_of(tl0) == D_INF as int);
                    assert(picked_of(tl0) =~= Seq::<Seq<u8>>::empty());
                    assert(thr_of(thr_new, fy as int, (*uom)@) == D_INF as int);
                    assert(thr_of(thr_new, fy as int, (*uom)@) == pivot_of(tl0));
                    assert forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (prev_out.at((0int, a)) is Some || picked_of(tl0).contains(a)) by {
                        assert(out.at((0int, a)) == prev_out.at((0int, a)));
                    }
                }
            }
            Some(inner) => {
                proof { assert(cells.at(ckey.view())->0 == *inner); assert(inner.wf()); assert(class_entries(*cells, fy as int, (*uom)@) == inner.entries()); }
                let (picks, piv) = scan_class(&*inner, dcur, &tset, Ghost(tkv));
                let mut q: usize = 0;
                let ghost pv = pick_view(picks@);
                while q < picks.len()
                    invariant
                        q <= picks@.len(),
                        pv == pick_view(picks@),
                        out.wf(),
                        forall|k: KeyView| #[trigger] out.at(k) is Some ==> k.0 == 0,
                        forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (prev_out.at((0int, a)) is Some || pv.take(q as int).contains(a)),
                    decreases picks.len() - q,
                {
                    out.put(Key::shared(0, picks[q].clone()), Arc::new(true));
                    proof {
                        assert(pv.take(q as int + 1) =~= pv.take(q as int).push(pv[q as int]));
                        assert forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (prev_out.at((0int, a)) is Some || pv.take(q as int + 1).contains(a)) by {
                            crate::filing::lemma_push_contains2(pv.take(q as int), pv[q as int], a);
                        }
                        assert(pv[q as int] == (*picks@[q as int])@);
                    }
                    q += 1;
                }
                proof { assert(pv.take(pv.len() as int) =~= pv); }
                if piv == D_INF {
                    thr_new.remove(&ckey);
                } else {
                    thr_new.put(ckey.share(), Arc::new(piv));
                }
                proof {
                    let tl0 = tail_of(*cells, *thr, tkv, fy as int, (*uom)@);
                    assert(tl0 == tail_in(inner.entries(), dcur as int, tkv));
                    assert(piv as int == pivot_of(tl0));
                    assert(pv == picked_of(tl0));
                    assert(thr_of(thr_new, fy as int, (*uom)@) == piv as int);
                    assert(thr_of(thr_new, fy as int, (*uom)@) == pivot_of(tl0));
                    assert forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (prev_out.at((0int, a)) is Some || picked_of(tl0).contains(a)) by {}
                }
            }
        }
        proof {
            let c = (fy as int, (*uom)@);
            let tl0 = tail_of(*cells, *thr, tkv, fy as int, (*uom)@);
            assert(thr_of(thr_new, fy as int, (*uom)@) == pivot_of(tl0));
            assert forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (prev_out.at((0int, a)) is Some || picked_of(tl0).contains(a)) by {}
            assert(hk[i as int] == c);
            assert(hk.take(i as int + 1) =~= hk.take(i as int).push(hk[i as int]));
            assert(hk.take(i as int + 1).contains(c)) by { assert(hk.take(i as int + 1)[i as int] == c); }
            assert forall|fy2: int, uom2: Seq<u8>| #[trigger] thr_of(thr_new, fy2, uom2) == if hk.take(i as int + 1).contains((fy2, uom2)) { dnew(*cells, *dir, *thr, tkv, fy2, uom2) } else { thr_of(*thr, fy2, uom2) } by {
                crate::filing::lemma_push_contains2(hk.take(i as int), hk[i as int], (fy2, uom2));
                if (fy2, uom2) == c {
                    assert(fy2 == fy as int && uom2 == (*uom)@);
                    assert(cls_hit(*dir, *thr, tkv, fy2, uom2));
                    assert(dnew(*cells, *dir, *thr, tkv, fy2, uom2) == pivot_of(tail_of(*cells, *thr, tkv, fy2, uom2)));
                    assert(tail_of(*cells, *thr, tkv, fy2, uom2) == tl0);
                    assert(thr_of(thr_new, fy2, uom2) == pivot_of(tl0));
                } else {
                    assert(thr_new.at((fy2, uom2)) == prev_thr.at((fy2, uom2)));
                    assert(hk.take(i as int + 1).contains((fy2, uom2)) == hk.take(i as int).contains((fy2, uom2)));
                    assert(thr_of(thr_new, fy2, uom2) == thr_of(prev_thr, fy2, uom2));
                }
            }
            assert forall|a: Seq<u8>| #[trigger] out.at((0int, a)) is Some <==> (tkv.contains(a)
                || exists|fy2: int, uom2: Seq<u8>| hk.take(i as int + 1).contains((fy2, uom2)) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy2, uom2)).contains(a)) by {
                if out.at((0int, a)) is Some {
                    if !(prev_out.at((0int, a)) is Some) {
                        let tl0 = tail_of(*cells, *thr, tkv, fy as int, (*uom)@);
                        assert(picked_of(tl0).contains(a));
                        assert(hk.take(i as int + 1).contains(c));
                    } else if !tkv.contains(a) {
                        let (fy2, uom2) = choose|fy2: int, uom2: Seq<u8>| hk.take(i as int).contains((fy2, uom2)) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy2, uom2)).contains(a);
                        assert(hk.take(i as int + 1).contains((fy2, uom2)));
                    }
                }
                if tkv.contains(a) {}
                if exists|fy2: int, uom2: Seq<u8>| hk.take(i as int + 1).contains((fy2, uom2)) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy2, uom2)).contains(a) {
                    let (fy2, uom2) = choose|fy2: int, uom2: Seq<u8>| hk.take(i as int + 1).contains((fy2, uom2)) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy2, uom2)).contains(a);
                    if (fy2, uom2) == c {
                        assert(out.at((0int, a)) is Some);
                    } else {
                        assert(hk.take(i as int).contains((fy2, uom2)));
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert(hk.take(hk.len() as int) =~= hk);
        // every hit class is among hk, and every class of hk is a hit class
        assert forall|fy: int, uom: Seq<u8>| hk.contains((fy, uom)) <==> cls_hit(*dir, *thr, tkv, fy, uom) by {
            if hk.contains((fy, uom)) {
                let q = choose|q: int| 0 <= q < hk.len() && hk[q] == (fy, uom);
                assert(hits.entries()[q].0 == (fy, uom));
                assert(hits.entries().contains(hits.entries()[q]));
                assert(hits.at((fy, uom)) is Some);
            }
            if cls_hit(*dir, *thr, tkv, fy, uom) {
                assert(hits.at((fy, uom)) is Some);
                assert(hits.view().dom().contains((fy, uom)));
                let row = (( fy, uom), hits.view()[(fy, uom)]);
                assert(hits.entries().contains(row));
                let q = choose|q: int| 0 <= q < hits.entries().len() && hits.entries()[q] == row;
                assert(hk[q] == (fy, uom));
            }
        }
    }
    // the result keys, sorted
    let orows = out.rows();
    proof { out.map_model(); }
    let mut keys: Vec<Vec<u8>> = Vec::new();
    let mut m: usize = 0;
    let ghost oe = out.entries();
    proof { assert(orows@.map_values(|row: (Key, Arc<bool>)| (row.0.view(), *row.1)) == oe); }
    while m < orows.len()
        invariant
            m <= orows@.len(),
            orows@.map_values(|row: (Key, Arc<bool>)| (row.0.view(), *row.1)) == oe,
            seq_view_bytes(keys@) == oe.take(m as int).map_values(|e: (KeyView, bool)| e.0.1),
        decreases orows.len() - m,
    {
        keys.push(copy_vec(orows[m].0.text.as_slice()));
        proof {
            assert(oe.take(m as int + 1) =~= oe.take(m as int).push(oe[m as int]));
            assert(oe[m as int] == (orows@[m as int].0.view(), *orows@[m as int].1));
            assert(oe[m as int].0 == orows@[m as int].0.view());
            assert(seq_view_bytes(keys@) =~= seq_view_bytes(keys@.drop_last()).push((*orows@[m as int].0.text)@));
            assert(oe.take(m as int + 1).map_values(|e: (KeyView, bool)| e.0.1) =~= oe.take(m as int).map_values(|e: (KeyView, bool)| e.0.1).push(oe[m as int].0.1));
            assert(seq_view_bytes(keys@) =~= oe.take(m as int + 1).map_values(|e: (KeyView, bool)| e.0.1));
        }
        m += 1;
    }
    proof {
        assert(oe.take(oe.len() as int) =~= oe);
        let q = seq_view_bytes(keys@);
        assert(q == oe.map_values(|e: (KeyView, bool)| e.0.1));
        assert(ordered_rows(oe));
        out.map_model();
        assert forall|i1: int, j1: int| 0 <= i1 < j1 < q.len() implies bytes_order()(#[trigger] q[i1], #[trigger] q[j1]) by {
            assert(oe.contains(oe[i1]));
            assert(oe.contains(oe[j1]));
            assert(key_lt(oe[i1].0, oe[j1].0));
            assert(oe[i1].0.0 == 0 && oe[j1].0.0 == 0) by {
                assert(out.at(oe[i1].0) is Some);
                assert(out.at(oe[j1].0) is Some);
            }
        }
        assert forall|a: Seq<u8>| q.contains(a) <==> wanted_has(*cells, *dir, *thr, tkv, a) by {
            if q.contains(a) {
                let p = choose|p: int| 0 <= p < q.len() && q[p] == a;
                assert(oe.contains(oe[p]));
                assert(out.at(oe[p].0) is Some);
                assert(oe[p].0 == (0int, a));
            }
            if out.at((0int, a)) is Some {
                out.map_model();
                let row = ((0int, a), out.view()[(0int, a)]);
                assert(oe.contains(row));
                let p = choose|p: int| 0 <= p < oe.len() && oe[p] == row;
                assert(q[p] == a);
            }
            if wanted_has(*cells, *dir, *thr, tkv, a) {
                if !tkv.contains(a) {
                    let (fy2, uom2) = choose|fy2: int, uom2: Seq<u8>| cls_hit(*dir, *thr, tkv, fy2, uom2) && picked_of(#[trigger] tail_of(*cells, *thr, tkv, fy2, uom2)).contains(a);
                    assert(hk.take(hk.len() as int).contains((fy2, uom2)));
                }
            }
        }
        assert forall|fy: int, uom: Seq<u8>| thr_of(thr_new, fy, uom) == dnew(*cells, *dir, *thr, tkv, fy, uom) by {
            if !hk.contains((fy, uom)) {
                assert(!cls_hit(*dir, *thr, tkv, fy, uom));
            }
        }
    }
    (keys, thr_new)
}

} // verus!
