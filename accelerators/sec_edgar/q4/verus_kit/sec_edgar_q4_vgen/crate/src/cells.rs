// The aggregate state: per (uom, stmt) an ordered index of the groups (sic, tlabel), each with its
// exact count, sum and sum of magnitudes (Wide) and the multiset of its companies.  A group is stored
// exactly when it has a nonzero aggregate; `rep` says the stored groups are those of a total `tot`
// (the sum of any group function over the tuples, for every parameter set).
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::abs;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::gen_obligations::params;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::keys::*;
use crate::rec::*;

verus! {

pub struct Cell {
    pub sic: i64,
    pub tlabel: Option<Vec<u8>>,
    pub count: Wide,
    pub sum: Wide,
    pub abs: Wide,
    pub ciks: Index<Wide>,
    pub nciks: u128,
    pub cs: Ghost<Set<int>>,
}

pub type Tot = spec_fn(Params, spec_fn(JK) -> int) -> int;

pub open spec fn tot_plus(tot: Tot, r: Rec) -> Tot {
    |p: Params, h: spec_fn(JK) -> int| tot(p, h) + rterm(p, h, r)
}

pub open spec fn q_one() -> spec_fn(JK) -> int {
    |j: JK| 1int
}

pub open spec fn q_val() -> spec_fn(JK) -> int {
    |j: JK| j.v
}

pub open spec fn q_abs() -> spec_fn(JK) -> int {
    |j: JK| abs(j.v)
}

pub open spec fn q_cik(c0: int) -> spec_fn(JK) -> int {
    |j: JK| if j.cik == Some(c0) { 1int } else { 0int }
}

/// f over the group (sic, tlabel) only.
pub open spec fn hf(sic: int, tl: Option<Seq<u8>>, f: spec_fn(JK) -> int) -> spec_fn(JK) -> int {
    |j: JK| if j.sic == Some(sic) && j.tlabel == tl { f(j) } else { 0 }
}

/// The parameters whose tuples are those of the groups at sic.
pub open spec fn gp(sic: int, stmt: Seq<u8>, uom: Seq<u8>) -> Params {
    params(sic, sic, stmt, uom)
}

pub open spec fn ck(c0: int) -> KeyView {
    (c0, Seq::<u8>::empty())
}

pub open spec fn flag<A>(x: Option<A>) -> u8 {
    if x is Some { 1u8 } else { 0u8 }
}

/// The key of group (sic, tlabel) in the groups of a (uom, stmt).
pub open spec fn gkey(sic: int, tl: Option<Seq<u8>>) -> KeyView {
    (sic, seq![flag(tl)] + match tl { Some(b) => b, None => Seq::empty() })
}

pub proof fn lemma_gkey_inj(sic: int, tl: Option<Seq<u8>>, sic2: int, tl2: Option<Seq<u8>>)
    requires gkey(sic, tl) == gkey(sic2, tl2),
    ensures sic == sic2, tl == tl2,
{
    let (a, b) = (gkey(sic, tl).1, gkey(sic2, tl2).1);
    assert(a[0] == flag(tl) && b[0] == flag(tl2));
    if tl is Some && tl2 is Some {
        assert(a.skip(1) =~= tl->0);
        assert(b.skip(1) =~= tl2->0);
    }
}

pub open spec fn touches(r: Rec, uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>) -> bool {
    r.uom@ == uom && r.stmt@ == stmt && r.sic as int == sic && opt_bytes(r.tlabel) == tl
}

/// A record adds w times f(its row) to the group it touches, and nothing to the others.
pub proof fn lemma_contrib(r: Rec, uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>, f: spec_fn(JK) -> int)
    ensures
        rterm(gp(sic, stmt, uom), hf(sic, tl, f), r)
            == if touches(r, uom, stmt, sic, tl) {
                r.w as int * f(JK { sic: Some(sic), tlabel: tl, cik: opt_int(r.cik), v: r.v@, vs: true, stmt: Some(stmt) })
            } else { 0 },
{
}

pub open spec fn cell_fields(c: Cell, tot: Tot, uom: Seq<u8>, stmt: Seq<u8>) -> bool {
    let sic = c.sic as int;
    let tl = opt_bytes(c.tlabel);
    let p = gp(sic, stmt, uom);
    &&& c.count@ == tot(p, hf(sic, tl, q_one()))
    &&& c.sum@ == tot(p, hf(sic, tl, q_val()))
    &&& c.abs@ == tot(p, hf(sic, tl, q_abs()))
    &&& c.ciks.wf()
    &&& c.nciks as int == c.cs@.len()
    &&& forall|c0: int| #[trigger] c.cs@.contains(c0) == c.ciks.at(ck(c0)).is_some()
    &&& forall|c0: int| #[trigger] c.cs@.contains(c0) ==> i64::MIN <= c0 <= i64::MAX
    &&& forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is Some ==>
            c.ciks.at(ck(c0))->0@ == tot(p, hf(sic, tl, q_cik(c0))) && c.ciks.at(ck(c0))->0@ != 0
    &&& forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is None ==> tot(p, hf(sic, tl, q_cik(c0))) == 0
}

pub open spec fn cell_nonzero(c: Cell) -> bool {
    !(c.count@ == 0 && c.sum@ == 0 && c.abs@ == 0 && c.nciks == 0)
}

pub open spec fn zero_rep(tot: Tot, uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>) -> bool {
    let p = gp(sic, stmt, uom);
    &&& tot(p, hf(sic, tl, q_one())) == 0
    &&& tot(p, hf(sic, tl, q_val())) == 0
    &&& tot(p, hf(sic, tl, q_abs())) == 0
    &&& forall|c0: int| #[trigger] tot(p, hf(sic, tl, q_cik(c0))) == 0
}

pub open spec fn cell_at(g: Index<Index<Cell>>, uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>) -> Option<Cell> {
    match g.at(key2(uom, stmt)) { Some(inner) => inner.at(gkey(sic, tl)), None => None }
}

pub open spec fn inner_ok(ii: Index<Cell>) -> bool {
    &&& ii.wf()
    &&& forall|k: KeyView| #[trigger] ii.at(k) is Some ==> k == gkey(ii.at(k)->0.sic as int, opt_bytes(ii.at(k)->0.tlabel))
}

pub open spec fn rep(g: Index<Index<Cell>>, tot: Tot) -> bool {
    &&& g.wf()
    &&& forall|k: KeyView| #[trigger] g.at(k) is Some ==> inner_ok(g.at(k)->0)
    &&& forall|uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>| #[trigger] cell_at(g, uom, stmt, sic, tl) is Some ==> {
        let c = cell_at(g, uom, stmt, sic, tl)->0;
        cell_fields(c, tot, uom, stmt) && cell_nonzero(c) && c.sic as int == sic && opt_bytes(c.tlabel) == tl
    }
    &&& forall|uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>| #[trigger] cell_at(g, uom, stmt, sic, tl) is None ==>
        zero_rep(tot, uom, stmt, sic, tl)
}

/// Fields of a cell are the same under a total that differs only by records not touching its group.
proof fn lemma_fields_same(c: Cell, tot: Tot, r: Rec, uom: Seq<u8>, stmt: Seq<u8>)
    requires
        cell_fields(c, tot, uom, stmt),
        !touches(r, uom, stmt, c.sic as int, opt_bytes(c.tlabel)),
    ensures
        cell_fields(c, tot_plus(tot, r), uom, stmt),
{
    let (sic, tl) = (c.sic as int, opt_bytes(c.tlabel));
    let p = gp(sic, stmt, uom);
    let t2 = tot_plus(tot, r);
    lemma_contrib(r, uom, stmt, sic, tl, q_one());
    lemma_contrib(r, uom, stmt, sic, tl, q_val());
    lemma_contrib(r, uom, stmt, sic, tl, q_abs());
    assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is Some implies
        c.ciks.at(ck(c0))->0@ == t2(p, hf(sic, tl, q_cik(c0))) && c.ciks.at(ck(c0))->0@ != 0 by {
        lemma_contrib(r, uom, stmt, sic, tl, q_cik(c0));
    }
    assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is None implies t2(p, hf(sic, tl, q_cik(c0))) == 0 by {
        lemma_contrib(r, uom, stmt, sic, tl, q_cik(c0));
    }
}

proof fn lemma_zero_same(tot: Tot, r: Rec, uom: Seq<u8>, stmt: Seq<u8>, sic: int, tl: Option<Seq<u8>>)
    requires
        zero_rep(tot, uom, stmt, sic, tl),
        !touches(r, uom, stmt, sic, tl),
    ensures
        zero_rep(tot_plus(tot, r), uom, stmt, sic, tl),
{
    let p = gp(sic, stmt, uom);
    let t2 = tot_plus(tot, r);
    lemma_contrib(r, uom, stmt, sic, tl, q_one());
    lemma_contrib(r, uom, stmt, sic, tl, q_val());
    lemma_contrib(r, uom, stmt, sic, tl, q_abs());
    assert forall|c0: int| #[trigger] t2(p, hf(sic, tl, q_cik(c0))) == 0 by {
        lemma_contrib(r, uom, stmt, sic, tl, q_cik(c0));
    }
}


fn gkey_exec(sic: i64, tl: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == gkey(sic as int, opt_bytes(*tl)),
{
    let mut text: Vec<u8> = Vec::new();
    text.push(if tl.is_some() { 1 } else { 0 });
    if let Some(b) = tl {
        let mut i: usize = 0;
        while i < b.len()
            invariant
                i <= b.len(),
                text@ == seq![1u8] + b@.take(i as int),
            decreases b.len() - i,
        {
            text.push(b[i]);
            proof { assert(text@ =~= seq![1u8] + b@.take(i as int + 1)); }
            i += 1;
        }
        proof { assert(b@.take(b.len() as int) =~= b@); }
    }
    Key::composite(sic, text)
}

fn copy_bytes(b: &[u8]) -> (out: Vec<u8>)
    ensures out@ == b@,
{
    vstd::slice::slice_to_vec(b)
}

fn copy_cell(c: &Cell) -> (r: Cell)
    ensures
        r.sic == c.sic,
        opt_bytes(r.tlabel) == opt_bytes(c.tlabel),
        r.count@ == c.count@, r.sum@ == c.sum@, r.abs@ == c.abs@,
        r.ciks.wf() == c.ciks.wf(),
        r.ciks.view() == c.ciks.view(),
        forall|k: KeyView| #[trigger] r.ciks.at(k) == c.ciks.at(k),
        r.nciks == c.nciks,
        r.cs@ == c.cs@,
{
    Cell {
        sic: c.sic,
        tlabel: match &c.tlabel { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
        count: c.count.dup(),
        sum: c.sum.dup(),
        abs: c.abs.dup(),
        ciks: c.ciks.share(),
        nciks: c.nciks,
        cs: Ghost(c.cs@),
    }
}

fn empty_cell(sic: i64, tl: &Option<Vec<u8>>) -> (r: Cell)
    ensures
        r.sic == sic,
        opt_bytes(r.tlabel) == opt_bytes(*tl),
        r.count@ == 0, r.sum@ == 0, r.abs@ == 0,
        r.ciks.wf(),
        forall|k: KeyView| #[trigger] r.ciks.at(k).is_none(),
        r.nciks == 0,
        r.cs@ == Set::<int>::empty(),
{
    Cell {
        sic,
        tlabel: match tl { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
        count: Wide::zero(),
        sum: Wide::zero(),
        abs: Wide::zero(),
        ciks: Index::new(),
        nciks: 0,
        cs: Ghost(Set::empty()),
    }
}

/// Add the record to the cell of the group it touches.
fn cell_add(c: &mut Cell, r: &Rec, Ghost(tot): Ghost<Tot>)
    requires
        cell_fields(*old(c), tot, r.uom@, r.stmt@),
        old(c).sic == r.sic,
        opt_bytes(old(c).tlabel) == opt_bytes(r.tlabel),
    ensures
        cell_fields(*final(c), tot_plus(tot, *r), r.uom@, r.stmt@),
        final(c).sic == r.sic,
        opt_bytes(final(c).tlabel) == opt_bytes(r.tlabel),
{
    let ghost (sic, tl) = (r.sic as int, opt_bytes(r.tlabel));
    let ghost (uom, stmt) = (r.uom@, r.stmt@);
    let ghost p = gp(sic, stmt, uom);
    let ghost t2 = tot_plus(tot, *r);
    let ghost c_init = *c;
    proof {
        lemma_contrib(*r, uom, stmt, sic, tl, q_one());
        lemma_contrib(*r, uom, stmt, sic, tl, q_val());
        lemma_contrib(*r, uom, stmt, sic, tl, q_abs());
    }
    let wv = Wide::from_i128(r.w);
    c.count = c.count.add(&wv);
    let prod = wv.mul(&r.v);
    c.sum = c.sum.add(&prod);
    let mag = if r.v.is_neg() { r.v.neg() } else { r.v.dup() };
    let pa = wv.mul(&mag);
    c.abs = c.abs.add(&pa);
    proof { assert(abs(r.v@) == mag@); }
    let ghost cs0 = c.cs@;
    let ghost ciks0 = c.ciks;
    match r.cik {
        None => {
            proof {
                assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is Some implies
                    c.ciks.at(ck(c0))->0@ == t2(p, hf(sic, tl, q_cik(c0))) && c.ciks.at(ck(c0))->0@ != 0 by {
                    lemma_contrib(*r, uom, stmt, sic, tl, q_cik(c0));
                }
                assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is None implies t2(p, hf(sic, tl, q_cik(c0))) == 0 by {
                    lemma_contrib(*r, uom, stmt, sic, tl, q_cik(c0));
                }
            }
        },
        Some(c0) => {
            let key = Key::number(c0);
            let found = c.ciks.get(&key);
            let old_m = match &found { Some(a) => a.dup(), None => Wide::zero() };
            let m = old_m.add(&wv);
            proof {
                lemma_contrib(*r, uom, stmt, sic, tl, q_cik(c0 as int));
                if found is Some {
                    assert(ciks0.at(ck(c0 as int)) == Some(*found->0));
                }
            }
            if m.is_zero() {
                if found.is_some() {
                    c.ciks.remove(&key);
                    c.nciks -= 1;
                    c.cs = Ghost(cs0.remove(c0 as int));
                }
            } else {
                if found.is_none() {
                    proof {
                        let range = Set::<int>::range(i64::MIN as int, i64::MAX as int + 1);
                        vstd::set_lib::lemma_int_range(i64::MIN as int, i64::MAX as int + 1);
                        assert(cs0.subset_of(range)) by {
                            assert forall|x: int| cs0.contains(x) implies range.contains(x) by {
                                assert(cell_fields(c_init, tot, uom, stmt));
                            }
                        }
                        vstd::set_lib::lemma_len_subset(cs0, range);
                    }
                    c.nciks += 1;
                    c.cs = Ghost(cs0.insert(c0 as int));
                }
                c.ciks.put(key.share(), Arc::new(m));
            }
            proof {
                assert forall|c1: int| #[trigger] c.ciks.at(ck(c1)) is Some implies
                    c.ciks.at(ck(c1))->0@ == t2(p, hf(sic, tl, q_cik(c1))) && c.ciks.at(ck(c1))->0@ != 0 by {
                    lemma_contrib(*r, uom, stmt, sic, tl, q_cik(c1));
                    if c1 != c0 as int { assert(ciks0.at(ck(c1)) == c.ciks.at(ck(c1))); }
                }
                assert forall|c1: int| #[trigger] c.ciks.at(ck(c1)) is None implies t2(p, hf(sic, tl, q_cik(c1))) == 0 by {
                    lemma_contrib(*r, uom, stmt, sic, tl, q_cik(c1));
                    if c1 != c0 as int { assert(ciks0.at(ck(c1)) == c.ciks.at(ck(c1))); }
                }
            }
        },
    }
}


/// The cell is the zero group (nothing stored).
fn cell_is_zero(c: &Cell) -> (z: bool)
    ensures z == !cell_nonzero(*c),
{
    c.count.is_zero() && c.sum.is_zero() && c.abs.is_zero() && c.nciks == 0
}

/// A zero cell's group has zero totals.
proof fn lemma_cell_zero_rep(c: Cell, tot: Tot, uom: Seq<u8>, stmt: Seq<u8>)
    requires cell_fields(c, tot, uom, stmt), !cell_nonzero(c),
    ensures zero_rep(tot, uom, stmt, c.sic as int, opt_bytes(c.tlabel)),
{
    let (sic, tl) = (c.sic as int, opt_bytes(c.tlabel));
    let p = gp(sic, stmt, uom);
    assert(c.cs@.len() == 0);
    assert forall|c0: int| #[trigger] tot(p, hf(sic, tl, q_cik(c0))) == 0 by {
        assert(!c.cs@.contains(c0));
        assert(c.ciks.at(ck(c0)).is_none());
    }
}

/// The groups with a record applied.
pub fn apply_rec(g: &mut Index<Index<Cell>>, r: &Rec, Ghost(tot): Ghost<Tot>)
    requires rep(*old(g), tot),
    ensures rep(*final(g), tot_plus(tot, *r)),
{
    let ghost g0 = *g;
    let ghost (uom, stmt) = (r.uom@, r.stmt@);
    let ghost (sic, tl) = (r.sic as int, opt_bytes(r.tlabel));
    let ghost t2 = tot_plus(tot, *r);
    let ok = make_key2(r.uom.as_slice(), r.stmt.as_slice());
    let ik = gkey_exec(r.sic, &r.tlabel);
    let mut inner: Index<Cell> = match g.get(&ok) {
        Some(a) => (*a).share(),
        None => Index::new(),
    };
    let ghost inner0 = inner;
    proof {
        match g0.at(ok.view()) {
            Some(i0) => {
                assert(inner_ok(i0));
                assert(inner0.view() == i0.view());
                assert forall|k: KeyView| inner0.at(k) == i0.at(k) by {}
            },
            None => {
                assert forall|k: KeyView| inner0.at(k).is_none() by {}
            },
        }
        assert(inner_ok(inner0));
        assert(forall|sc: int, st: Option<Seq<u8>>| cell_at(g0, uom, stmt, sc, st) == inner0.at(gkey(sc, st)));
    }
    let found = inner.get(&ik);
    let mut c: Cell = match &found {
        Some(a) => copy_cell(&**a),
        None => empty_cell(r.sic, &r.tlabel),
    };
    proof {
        match found {
            Some(a) => {
                assert(inner0.at(ik.view()) == Some(*a));
                assert(cell_at(g0, uom, stmt, sic, tl) == Some(*a));
                assert(cell_fields(*a, tot, uom, stmt));
                assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) == a.ciks.at(ck(c0)) by {}
                assert(cell_fields(c, tot, uom, stmt));
            },
            None => {
                assert(cell_at(g0, uom, stmt, sic, tl) is None);
                assert(zero_rep(tot, uom, stmt, sic, tl));
                assert(c.cs@ == Set::<int>::empty());
                assert forall|c0: int| #[trigger] c.cs@.contains(c0) == c.ciks.at(ck(c0)).is_some() by {
                    assert(!Set::<int>::empty().contains(c0));
                    assert(c.ciks.at(ck(c0)).is_none());
                }
                assert(c.cs@.len() == 0);
                let pp = gp(c.sic as int, stmt, uom);
                assert(c.count@ == tot(pp, hf(c.sic as int, opt_bytes(c.tlabel), q_one())));
                assert(c.sum@ == tot(pp, hf(c.sic as int, opt_bytes(c.tlabel), q_val())));
                assert(c.abs@ == tot(pp, hf(c.sic as int, opt_bytes(c.tlabel), q_abs())));
                assert(c.ciks.wf());
                assert(c.nciks as int == c.cs@.len());
                assert forall|c0: int| #[trigger] c.cs@.contains(c0) implies i64::MIN <= c0 <= i64::MAX by {
                    assert(!c.cs@.contains(c0));
                }
                assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is None implies tot(pp, hf(c.sic as int, opt_bytes(c.tlabel), q_cik(c0))) == 0 by {}
                assert forall|c0: int| #[trigger] c.ciks.at(ck(c0)) is Some implies
                    c.ciks.at(ck(c0))->0@ == tot(pp, hf(c.sic as int, opt_bytes(c.tlabel), q_cik(c0))) && c.ciks.at(ck(c0))->0@ != 0 by {
                    assert(c.ciks.at(ck(c0)).is_none());
                }
                assert(cell_fields(c, tot, uom, stmt));
            },
        }
    }
    cell_add(&mut c, r, Ghost(tot));
    let removed = cell_is_zero(&c);
    let ghost cell_new = c;
    if removed {
        inner.remove(&ik);
    } else {
        inner.put(ik.share(), Arc::new(c));
    }
    g.put(ok.share(), Arc::new(inner));
    proof {
        let ghost g1 = *g;
        assert(g1.wf());
        assert forall|k: KeyView| #[trigger] g1.at(k) is Some implies inner_ok(g1.at(k)->0) by {
            if k == ok.view() {
                assert(g1.at(k) == Some(inner));
                assert forall|k2: KeyView| #[trigger] inner.at(k2) is Some implies
                    k2 == gkey(inner.at(k2)->0.sic as int, opt_bytes(inner.at(k2)->0.tlabel)) by {
                    if k2 == ik.view() {
                    } else {
                        assert(inner.at(k2) == inner0.at(k2));
                    }
                }
            } else {
                assert(g1.at(k) == g0.at(k));
            }
        }
        assert forall|u: Seq<u8>, st: Seq<u8>, sc: int, tl2: Option<Seq<u8>>| #[trigger] cell_at(g1, u, st, sc, tl2) is Some implies {
            let cc = cell_at(g1, u, st, sc, tl2)->0;
            cell_fields(cc, t2, u, st) && cell_nonzero(cc) && cc.sic as int == sc && opt_bytes(cc.tlabel) == tl2
        } by {
            if key2(u, st) == ok.view() {
                lemma_key2_inj(u, st, uom, stmt);
                assert(g1.at(key2(u, st)) == Some(inner));
                if gkey(sc, tl2) == ik.view() {
                    lemma_gkey_inj(sc, tl2, sic, tl);
                    assert(!removed);
                    assert(inner.at(ik.view()) == Some(cell_new));
                } else {
                    let cc = inner0.at(gkey(sc, tl2))->0;
                    assert(inner.at(gkey(sc, tl2)) == inner0.at(gkey(sc, tl2)));
                    assert(cell_at(g0, u, st, sc, tl2) == inner0.at(gkey(sc, tl2)));
                    assert(inner_ok(inner0));
                    lemma_gkey_inj(sc, tl2, cc.sic as int, opt_bytes(cc.tlabel));
                    lemma_fields_same(cc, tot, *r, u, st);
                }
            } else {
                assert(cell_at(g1, u, st, sc, tl2) == cell_at(g0, u, st, sc, tl2));
                let cc = cell_at(g0, u, st, sc, tl2)->0;
                assert(!(u == uom && st == stmt));
                lemma_fields_same(cc, tot, *r, u, st);
            }
        }
        assert forall|u: Seq<u8>, st: Seq<u8>, sc: int, tl2: Option<Seq<u8>>| #[trigger] cell_at(g1, u, st, sc, tl2) is None implies
            zero_rep(t2, u, st, sc, tl2) by {
            if key2(u, st) == ok.view() {
                lemma_key2_inj(u, st, uom, stmt);
                assert(g1.at(key2(u, st)) == Some(inner));
                if gkey(sc, tl2) == ik.view() {
                    lemma_gkey_inj(sc, tl2, sic, tl);
                    assert(removed);
                    lemma_cell_zero_rep(cell_new, t2, uom, stmt);
                } else {
                    assert(inner.at(gkey(sc, tl2)) == inner0.at(gkey(sc, tl2)));
                    assert(cell_at(g0, u, st, sc, tl2) is None);
                    lemma_zero_same(tot, *r, u, st, sc, tl2);
                }
            } else {
                assert(cell_at(g0, u, st, sc, tl2) is None);
                lemma_zero_same(tot, *r, u, st, sc, tl2);
            }
        }
    }
}


/// Totals that agree on every parameter set and group function represent the same groups.
pub proof fn lemma_rep_ext(g: Index<Index<Cell>>, t1: Tot, t2: Tot)
    requires forall|p: Params, h: spec_fn(JK) -> int| #[trigger] t1(p, h) == t2(p, h),
    ensures rep(g, t1) == rep(g, t2),
{
    assert(t1 =~= t2);
}

}
