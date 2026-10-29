// The order of a class's groups by rounded total, descending, as an Index key: the total's bits mapped
// to an i64 that increases as the value decreases, then the group's company number and key.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Key, KeyView};
use crate::ent::*;
use crate::f64bits::*;
use crate::gkey::*;
use verus_prooflib::ordered::Index;
use verus_prooflib::wide::Wide;

verus! {

/// The bits of a finite double as an integer that increases as the value decreases.
pub open spec fn rank(b: u64) -> int {
    if b as int / pow2(63) == 1 { b as int - pow2(63) } else { -1 - b as int }
}

proof fn lemma_mag_mono(a: u64, b: u64)
    requires f64_finite(a), f64_finite(b), f64_mag_bits(a) < f64_mag_bits(b),
    ensures f64_mag(a) <= f64_mag(b),
{
    lemma_pow2_consts();
    lemma_decompose(a);
    lemma_decompose(b);
    let (ea, eb, fa, fb) = (f64_exp(a), f64_exp(b), f64_frac(a), f64_frac(b));
    assert(ea <= eb && (ea < eb || fa < fb)) by (nonlinear_arith)
        requires f64_mag_bits(a) == ea * pow2(52) + fa, f64_mag_bits(b) == eb * pow2(52) + fb,
            f64_mag_bits(a) < f64_mag_bits(b), 0 <= fa < pow2(52), 0 <= fb < pow2(52), 0 <= ea, 0 <= eb, pow2(52) > 0;
    if ea == eb {
        if ea != 0 {
            lemma_pow2_pos((ea - 1) as nat);
            assert((fa + pow2(52)) * pow2((ea - 1) as nat) <= (fb + pow2(52)) * pow2((ea - 1) as nat)) by (nonlinear_arith)
                requires fa < fb, pow2((ea - 1) as nat) > 0;
        }
    } else {
        lemma_pow2_pos((eb - 1) as nat);
        let pb = pow2((eb - 1) as nat);
        assert(pb >= 1);
        if ea == 0 {
            assert((fb + pow2(52)) * pb >= pow2(52)) by (nonlinear_arith) requires fb >= 0, pb >= 1, pow2(52) > 0;
        } else {
            lemma_pow2_pos((ea - 1) as nat);
            lemma_pow2_add(ea as nat, 0);
            lemma_pow2_mono(ea as nat, (eb - 1) as nat);
            let pa = pow2((ea - 1) as nat);
            assert(pow2(ea as nat) == 2 * pa) by { reveal_with_fuel(pow2, 2); lemma_pow2_add((ea - 1) as nat, 1); }
            assert((fa + pow2(52)) * pa < pow2(52) * pow2(ea as nat)) by (nonlinear_arith)
                requires fa < pow2(52), pa > 0, pow2(ea as nat) == 2 * pa;
            assert(pow2(52) * pow2(ea as nat) <= pow2(52) * pb) by (nonlinear_arith)
                requires pow2(ea as nat) <= pb, pow2(52) > 0;
            assert(pow2(52) * pb <= (fb + pow2(52)) * pb) by (nonlinear_arith) requires fb >= 0, pb >= 1;
        }
    }
}

/// A greater value has a smaller rank.
pub proof fn lemma_rank_order(a: u64, b: u64)
    requires f64_finite(a), f64_finite(b), f64_val(a) > f64_val(b),
    ensures rank(a) < rank(b),
{
    lemma_pow2_consts();
    lemma_decompose(a);
    lemma_decompose(b);
    let (sa, sb) = (a as int / pow2(63), b as int / pow2(63));
    assert(f64_val(a) == if sa == 1 { -f64_mag(a) } else { f64_mag(a) });
    assert(f64_val(b) == if sb == 1 { -f64_mag(b) } else { f64_mag(b) });
    let (ma, mb) = (f64_mag_bits(a), f64_mag_bits(b));
    if sa == 0 && sb == 0 {
        if ma <= mb {
            if ma < mb { lemma_mag_mono(a, b); } else { lemma_mag_inj_eq(a, b); }
            assert(false);
        }
    } else if sa == 1 && sb == 1 {
        if ma >= mb {
            if ma > mb { lemma_mag_mono(b, a); } else { lemma_mag_inj_eq(a, b); }
            assert(false);
        }
    } else if sa == 1 {
        lemma_mag_nonneg(a);
        lemma_mag_nonneg(b);
        assert(false);
    }
}

proof fn lemma_mag_nonneg(b: u64)
    requires f64_finite(b),
    ensures f64_mag(b) >= 0,
{
    lemma_pow2_consts();
    if f64_exp(b) != 0 {
        lemma_pow2_pos((f64_exp(b) - 1) as nat);
        let (f, p) = (f64_frac(b), pow2((f64_exp(b) - 1) as nat));
        assert((f + pow2(52)) * p >= 0) by (nonlinear_arith) requires f >= 0, p > 0, pow2(52) > 0;
    }
}

proof fn lemma_mag_inj_eq(a: u64, b: u64)
    requires f64_finite(a), f64_finite(b), f64_mag_bits(a) == f64_mag_bits(b),
    ensures f64_mag(a) == f64_mag(b),
{
    lemma_pow2_consts();
    lemma_decompose(a);
    lemma_decompose(b);
    vstd::arithmetic::div_mod::lemma_mod_bound(a as int, pow2(52));
    vstd::arithmetic::div_mod::lemma_mod_bound(b as int, pow2(52));
    assert(f64_exp(a) == f64_exp(b) && f64_frac(a) == f64_frac(b)) by (nonlinear_arith)
        requires f64_mag_bits(a) == f64_exp(a) * pow2(52) + f64_frac(a), f64_mag_bits(b) == f64_exp(b) * pow2(52) + f64_frac(b),
            f64_mag_bits(a) == f64_mag_bits(b), 0 <= f64_frac(a) < pow2(52), 0 <= f64_frac(b) < pow2(52), pow2(52) > 0;
}


pub open spec fn be8(v: u64) -> Seq<u8> {
    seq![(v >> 56) as u8, (v >> 48) as u8, (v >> 40) as u8, (v >> 32) as u8, (v >> 24) as u8, (v >> 16) as u8, (v >> 8) as u8, v as u8]
}

proof fn lemma_be8_inj(v: u64, w: u64)
    requires be8(v) == be8(w),
    ensures v == w,
{
    let (a, b) = (be8(v), be8(w));
    assert(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5] && a[6] == b[6] && a[7] == b[7]);
    assert(v == w) by (bit_vector)
        requires (v >> 56) as u8 == (w >> 56) as u8, (v >> 48) as u8 == (w >> 48) as u8, (v >> 40) as u8 == (w >> 40) as u8,
            (v >> 32) as u8 == (w >> 32) as u8, (v >> 24) as u8 == (w >> 24) as u8, (v >> 16) as u8 == (w >> 16) as u8,
            (v >> 8) as u8 == (w >> 8) as u8, v as u8 == w as u8;
}

pub open spec fn cikv(c: Option<i64>) -> u64 {
    match c { Some(x) => x as u64, None => 0u64 }
}

pub open spec fn rank_e(e: Ent) -> int {
    match e.bits {
        Some(b) => rank(b),
        None => if e.cv@ < 0 { 9223372036854775807int } else { -9223372036854775808int },
    }
}

/// The key of a group in its class's order: by rounded total, descending; ties by company, then name.
pub open spec fn okey(e: Ent) -> KeyView {
    (rank_e(e), be8(cikv(e.cik)) + gk(opt_bytes(e.name), opt_int(e.cik)).1)
}

pub proof fn lemma_okey_inj(x: Ent, y: Ent)
    requires okey(x) == okey(y),
    ensures gk(opt_bytes(x.name), opt_int(x.cik)) == gk(opt_bytes(y.name), opt_int(y.cik)),
{
    let (tx, ty) = (okey(x).1, okey(y).1);
    let (gx, gy) = (gk(opt_bytes(x.name), opt_int(x.cik)).1, gk(opt_bytes(y.name), opt_int(y.cik)).1);
    assert(tx.take(8) =~= be8(cikv(x.cik)));
    assert(ty.take(8) =~= be8(cikv(y.cik)));
    assert(tx.skip(8) =~= gx);
    assert(ty.skip(8) =~= gy);
    lemma_be8_inj(cikv(x.cik), cikv(y.cik));
    assert(gx[0] == gy[0]);
    assert(gx[0] == flag(opt_int(x.cik)) && gy[0] == flag(opt_int(y.cik)));
    if x.cik is Some && y.cik is Some {
        let (p, q) = (x.cik->0, y.cik->0);
        assert(p as u64 == q as u64);
        assert(p == q) by (bit_vector) requires p as u64 == q as u64;
    }
    assert(opt_int(x.cik) == opt_int(y.cik));
    assert(opt_bytes(x.name) == opt_bytes(y.name) || true);
}

pub open spec fn oorw() -> spec_fn((KeyView, Ent)) -> int {
    |row: (KeyView, Ent)| if row.1.bits is None { 1int } else { 0int }
}

/// The order index holds exactly the entries of the map, each under its order key.
pub open spec fn ord_ok(m: Index<Ent>, o: Index<Ent>) -> bool {
    &&& o.wf()
    &&& forall|k: KeyView| #[trigger] m.at(k) is Some ==> o.at(okey(m.at(k)->0)) == m.at(k)
    &&& forall|q: KeyView| #[trigger] o.at(q) is Some ==>
        q == okey(o.at(q)->0) && m.at(gk(opt_bytes(o.at(q)->0.name), opt_int(o.at(q)->0.cik))) == o.at(q)
}

fn rank_exec(b: u64) -> (r: i64)
    requires f64_finite(b),
    ensures r as int == rank(b),
{
    proof { lemma_pow2_consts(); assert(b as int / pow2(63) == (b >> 63) as int) by (bit_vector); }
    if b >> 63 == 1 { (b - 0x8000_0000_0000_0000u64) as i64 } else { -1 - (b as i64) }
}

/// The order key of an entry.
pub fn ord_key(e: &Ent) -> (k: Key)
    requires e.bits matches Some(b) ==> f64_finite(b),
    ensures k.view() == okey(*e),
{
    let r: i64 = match e.bits {
        Some(b) => rank_exec(b),
        None => if e.cv.is_neg() { i64::MAX } else { i64::MIN },
    };
    let g = gk_key(&e.name, e.cik);
    let v: u64 = match e.cik { Some(x) => x as u64, None => 0 };
    let mut t: Vec<u8> = Vec::with_capacity(if g.text.len() < 1048576 { 8 + g.text.len() } else { 8 });
    t.push((v >> 56) as u8); t.push((v >> 48) as u8); t.push((v >> 40) as u8); t.push((v >> 32) as u8);
    t.push((v >> 24) as u8); t.push((v >> 16) as u8); t.push((v >> 8) as u8); t.push(v as u8);
    let mut i: usize = 0;
    while i < g.text.len()
        invariant i <= g.text.len(), t@ == be8(cikv(e.cik)) + (*g.text)@.take(i as int),
        decreases g.text.len() - i,
    {
        t.push(g.text[i]);
        proof { assert(t@ =~= be8(cikv(e.cik)) + (*g.text)@.take(i as int + 1)); }
        i += 1;
    }
    proof { assert((*g.text)@.take(g.text.len() as int) =~= (*g.text)@); assert(t@ =~= be8(cikv(e.cik)) + gk(opt_bytes(e.name), opt_int(e.cik)).1); }
    Key::composite(r, t)
}


/// One group of the map replaced (old -> new) and its order key re-keyed.
pub proof fn lemma_ord_step(mb: Index<Ent>, ma: Index<Ent>, ob: Index<Ent>, oa: Index<Ent>, k: KeyView, old: Option<Ent>, new: Option<Ent>)
    requires
        ord_ok(mb, ob),
        mb.at(k) == old,
        forall|q: KeyView| #[trigger] mb.at(q) is Some ==> q == gk(opt_bytes(mb.at(q)->0.name), opt_int(mb.at(q)->0.cik)),
        forall|q: KeyView| #[trigger] ma.at(q) == if q == k { new } else { mb.at(q) },
        oa.wf(),
        forall|q: KeyView| #[trigger] oa.at(q) == if new is Some && q == okey(new->0) { new }
            else if old is Some && q == okey(old->0) { None::<Ent> } else { ob.at(q) },
        old is Some ==> k == gk(opt_bytes(old->0.name), opt_int(old->0.cik)),
        new is Some ==> k == gk(opt_bytes(new->0.name), opt_int(new->0.cik)),
    ensures ord_ok(ma, oa),
{
    assert forall|q: KeyView| #[trigger] ma.at(q) is Some implies oa.at(okey(ma.at(q)->0)) == ma.at(q) by {
        let x = ma.at(q)->0;
        if q == k {
            assert(oa.at(okey(x)) == new);
        } else {
            assert(mb.at(q) == Some(x));
            assert(ob.at(okey(x)) == Some(x));
            if new is Some && okey(x) == okey(new->0) { lemma_okey_inj(x, new->0); assert(q == k); }
            if old is Some && okey(x) == okey(old->0) { lemma_okey_inj(x, old->0); assert(q == k); }
        }
    }
    assert forall|r: KeyView| #[trigger] oa.at(r) is Some implies
        r == okey(oa.at(r)->0) && ma.at(gk(opt_bytes(oa.at(r)->0.name), opt_int(oa.at(r)->0.cik))) == oa.at(r) by {
        let x = oa.at(r)->0;
        if new is Some && r == okey(new->0) {
            assert(x == new->0);
            assert(ma.at(k) == new);
        } else {
            assert(ob.at(r) == Some(x));
            assert(r == okey(x));
            let g = gk(opt_bytes(x.name), opt_int(x.cik));
            assert(mb.at(g) == Some(x));
            if g == k { assert(old == Some(x)); assert(r == okey(old->0)); assert(false); }
            assert(ma.at(g) == mb.at(g));
        }
    }
}


pub proof fn lemma_ord_same(m1: &Index<Ent>, m2: &Index<Ent>, o1: &Index<Ent>, o2: &Index<Ent>)
    requires
        ord_ok(*m1, *o1),
        o2.wf(),
        forall|k: KeyView| #[trigger] m1.at(k) == m2.at(k),
        forall|k: KeyView| #[trigger] o1.at(k) == o2.at(k),
    ensures ord_ok(*m2, *o2),
{
    assert forall|k: KeyView| #[trigger] m2.at(k) is Some implies o2.at(okey(m2.at(k)->0)) == m2.at(k) by {
        assert(m1.at(k) == m2.at(k));
        assert(o1.at(okey(m1.at(k)->0)) == o2.at(okey(m1.at(k)->0)));
    }
    assert forall|q: KeyView| #[trigger] o2.at(q) is Some implies
        q == okey(o2.at(q)->0) && m2.at(gk(opt_bytes(o2.at(q)->0.name), opt_int(o2.at(q)->0.cik))) == o2.at(q) by {
        assert(o1.at(q) == o2.at(q));
        let g = gk(opt_bytes(o2.at(q)->0.name), opt_int(o2.at(q)->0.cik));
        assert(m1.at(g) == m2.at(g));
    }
}

}
