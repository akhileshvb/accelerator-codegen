// Self-delimiting byte-string encoding for composite ordered-index keys.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_prooflib::ordered::{Key, KeyView};

verus! {

/// Each byte b becomes [1, b]; 0 ends the string, so no encoding is a prefix of another.
pub open spec fn body(b: Seq<u8>) -> Seq<u8>
    decreases b.len(),
{
    if b.len() == 0 { Seq::empty() } else { body(b.drop_last()) + seq![1u8, b.last()] }
}

pub open spec fn esc(b: Seq<u8>) -> Seq<u8> {
    body(b) + seq![0u8]
}

/// NULL is the byte 2; text starts with 0 or 1.
pub open spec fn esc_opt(o: Option<Seq<u8>>) -> Seq<u8> {
    match o { Some(b) => esc(b), None => seq![2u8] }
}

pub open spec fn okey(fy: int, stmt: Seq<u8>, uom: Seq<u8>) -> KeyView {
    (fy, esc(stmt) + esc(uom))
}

pub open spec fn ikey(name: Option<Seq<u8>>, tag: Seq<u8>, plabel: Option<Seq<u8>>) -> KeyView {
    (0int, esc_opt(name) + esc(tag) + esc_opt(plabel))
}

pub proof fn lemma_body_head(b: Seq<u8>)
    requires b.len() > 0,
    ensures body(b) == seq![1u8, b[0]] + body(b.skip(1)),
    decreases b.len(),
{
    if b.len() > 1 {
        lemma_body_head(b.drop_last());
        assert(b.drop_last().skip(1) =~= b.skip(1).drop_last());
        assert(body(b) =~= seq![1u8, b[0]] + body(b.skip(1)));
    } else {
        assert(b.skip(1) =~= Seq::<u8>::empty());
        assert(b =~= seq![b[0]]);
        assert(b.drop_last() =~= Seq::<u8>::empty());
    }
}

/// Encodings are injective and self-delimiting.
pub proof fn lemma_esc_split(a: Seq<u8>, r1: Seq<u8>, b: Seq<u8>, r2: Seq<u8>)
    requires esc(a) + r1 == esc(b) + r2,
    ensures a == b, r1 == r2,
    decreases a.len(),
{
    if a.len() == 0 {
        assert(esc(a) =~= seq![0u8]);
        if b.len() > 0 {
            lemma_body_head(b);
            assert((esc(b) + r2)[0] == 1u8);
            assert(false);
        }
        assert(esc(b) =~= seq![0u8]);
        assert(b =~= a);
        assert((esc(a) + r1).skip(1) =~= r1);
        assert((esc(b) + r2).skip(1) =~= r2);
        assert(r1 =~= r2);
    } else {
        lemma_body_head(a);
        if b.len() == 0 {
            assert(esc(b) =~= seq![0u8]);
            assert((esc(a) + r1)[0] == 1u8);
            assert(false);
        }
        lemma_body_head(b);
        assert(esc(a) =~= seq![1u8, a[0]] + esc(a.skip(1)));
        assert(esc(b) =~= seq![1u8, b[0]] + esc(b.skip(1)));
        assert((esc(a) + r1)[1] == a[0]);
        assert((esc(b) + r2)[1] == b[0]);
        assert(a[0] == b[0]);
        assert((esc(a) + r1).skip(2) =~= esc(a.skip(1)) + r1);
        assert((esc(b) + r2).skip(2) =~= esc(b.skip(1)) + r2);
        lemma_esc_split(a.skip(1), r1, b.skip(1), r2);
        assert(a =~= seq![a[0]] + a.skip(1));
        assert(b =~= seq![b[0]] + b.skip(1));
    }
}

pub proof fn lemma_esc_opt_split(a: Option<Seq<u8>>, r1: Seq<u8>, b: Option<Seq<u8>>, r2: Seq<u8>)
    requires esc_opt(a) + r1 == esc_opt(b) + r2,
    ensures a == b, r1 == r2,
{
    match (a, b) {
        (Some(x), Some(y)) => { lemma_esc_split(x, r1, y, r2); }
        (None, None) => { assert((esc_opt(a) + r1).skip(1) =~= r1); assert((esc_opt(b) + r2).skip(1) =~= r2); assert(r1 =~= r2); }
        (Some(x), None) => {
            assert(esc(x)[0] == 0u8 || esc(x)[0] == 1u8) by {
                if x.len() > 0 { lemma_body_head(x); }
            }
            assert((esc_opt(a) + r1)[0] == esc(x)[0]);
            assert(false);
        }
        (None, Some(y)) => {
            assert(esc(y)[0] == 0u8 || esc(y)[0] == 1u8) by {
                if y.len() > 0 { lemma_body_head(y); }
            }
            assert((esc_opt(a) + r1)[0] == 2u8);
            assert((esc_opt(b) + r2)[0] == esc(y)[0]);
            assert(false);
        }
    }
}

pub proof fn lemma_okey_inj(f1: int, s1: Seq<u8>, u1: Seq<u8>, f2: int, s2: Seq<u8>, u2: Seq<u8>)
    requires okey(f1, s1, u1) == okey(f2, s2, u2),
    ensures f1 == f2, s1 == s2, u1 == u2,
{
    lemma_esc_split(s1, esc(u1), s2, esc(u2));
    lemma_esc_split(u1, Seq::empty(), u2, Seq::empty());
}

pub proof fn lemma_ikey_inj(n1: Option<Seq<u8>>, t1: Seq<u8>, p1: Option<Seq<u8>>, n2: Option<Seq<u8>>, t2: Seq<u8>, p2: Option<Seq<u8>>)
    requires ikey(n1, t1, p1) == ikey(n2, t2, p2),
    ensures n1 == n2, t1 == t2, p1 == p2,
{
    assert(esc_opt(n1) + (esc(t1) + esc_opt(p1)) =~= esc_opt(n1) + esc(t1) + esc_opt(p1));
    assert(esc_opt(n2) + (esc(t2) + esc_opt(p2)) =~= esc_opt(n2) + esc(t2) + esc_opt(p2));
    lemma_esc_opt_split(n1, esc(t1) + esc_opt(p1), n2, esc(t2) + esc_opt(p2));
    lemma_esc_split(t1, esc_opt(p1), t2, esc_opt(p2));
    lemma_esc_opt_split(p1, Seq::empty(), p2, Seq::empty());
}

pub fn append_esc(out: &mut Vec<u8>, b: &[u8])
    ensures final(out)@ == old(out)@ + esc(b@),
{
    let mut i = 0;
    while i < b.len()
        invariant
            i <= b.len(),
            out@ == old(out)@ + body(b@.take(i as int)),
        decreases b.len() - i,
    {
        out.push(1u8);
        out.push(b[i]);
        proof {
            let t = b@.take(i as int + 1);
            assert(t.drop_last() =~= b@.take(i as int));
            assert(t.last() == b[i as int]);
            assert(out@ =~= old(out)@ + body(t));
        }
        i += 1;
    }
    out.push(0u8);
    assert(b@.take(i as int) =~= b@);
    assert(out@ =~= old(out)@ + esc(b@));
}

pub fn append_esc_opt(out: &mut Vec<u8>, b: &Option<Vec<u8>>)
    ensures final(out)@ == old(out)@ + esc_opt(match b { Some(v) => Some(v@), None => None }),
{
    match b {
        Some(v) => append_esc(out, v.as_slice()),
        None => out.push(2u8),
    }
}

pub fn okey_of(fy: i64, stmt: &[u8], uom: &[u8]) -> (k: Key)
    ensures k.view() == okey(fy as int, stmt@, uom@),
{
    let mut text = Vec::new();
    append_esc(&mut text, stmt);
    append_esc(&mut text, uom);
    Key::composite(fy, text)
}

pub fn ikey_of(name: &Option<Vec<u8>>, tag: &[u8], plabel: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == ikey(match name { Some(v) => Some(v@), None => None }, tag@, match plabel { Some(v) => Some(v@), None => None }),
{
    let mut text = Vec::new();
    append_esc_opt(&mut text, name);
    append_esc(&mut text, tag);
    append_esc_opt(&mut text, plabel);
    proof { assert(text@ =~= esc_opt(match name { Some(v) => Some(v@), None => None }) + esc(tag@) + esc_opt(match plabel { Some(v) => Some(v@), None => None })); }
    Key::composite(0, text)
}

} // verus!
