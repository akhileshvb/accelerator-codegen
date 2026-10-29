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

pub open spec fn okey(y: int, uom: Seq<u8>) -> KeyView {
    (y, esc(uom))
}

pub open spec fn ikey(tag: Option<Seq<u8>>, ver: Option<Seq<u8>>) -> KeyView {
    (0int, esc_opt(tag) + esc_opt(ver))
}

/// The key of a filing's tag version: (adsh, tag, version).
pub open spec fn key3(a: Seq<u8>, t: Seq<u8>, v: Seq<u8>) -> KeyView {
    (0int, esc(a) + esc(t) + esc(v))
}

proof fn lemma_body_head(b: Seq<u8>)
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

pub proof fn lemma_okey_inj(y1: int, u1: Seq<u8>, y2: int, u2: Seq<u8>)
    requires okey(y1, u1) == okey(y2, u2),
    ensures y1 == y2, u1 == u2,
{
    lemma_esc_split(u1, Seq::empty(), u2, Seq::empty());
}

pub proof fn lemma_ikey_inj(t1: Option<Seq<u8>>, v1: Option<Seq<u8>>, t2: Option<Seq<u8>>, v2: Option<Seq<u8>>)
    requires ikey(t1, v1) == ikey(t2, v2),
    ensures t1 == t2, v1 == v2,
{
    lemma_esc_opt_split(t1, esc_opt(v1), t2, esc_opt(v2));
    lemma_esc_opt_split(v1, Seq::empty(), v2, Seq::empty());
}

pub proof fn lemma_key3_inj(a1: Seq<u8>, t1: Seq<u8>, v1: Seq<u8>, a2: Seq<u8>, t2: Seq<u8>, v2: Seq<u8>)
    requires key3(a1, t1, v1) == key3(a2, t2, v2),
    ensures a1 == a2, t1 == t2, v1 == v2,
{
    assert(esc(a1) + esc(t1) + esc(v1) =~= esc(a1) + (esc(t1) + esc(v1)));
    assert(esc(a2) + esc(t2) + esc(v2) =~= esc(a2) + (esc(t2) + esc(v2)));
    lemma_esc_split(a1, esc(t1) + esc(v1), a2, esc(t2) + esc(v2));
    lemma_esc_split(t1, esc(v1), t2, esc(v2));
    lemma_esc_split(v1, Seq::empty(), v2, Seq::empty());
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

pub fn okey_of(y: i64, uom: &[u8]) -> (k: Key)
    ensures k.view() == okey(y as int, uom@),
{
    let mut text = Vec::new();
    append_esc(&mut text, uom);
    Key::composite(y, text)
}

pub fn ikey_of(tag: &Option<Vec<u8>>, ver: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == ikey(match tag { Some(v) => Some(v@), None => None }, match ver { Some(v) => Some(v@), None => None }),
{
    let mut text = Vec::new();
    append_esc_opt(&mut text, tag);
    append_esc_opt(&mut text, ver);
    Key::composite(0, text)
}

pub fn key3_of(a: &[u8], t: &[u8], v: &[u8]) -> (k: Key)
    ensures k.view() == key3(a@, t@, v@),
{
    let mut text = Vec::new();
    append_esc(&mut text, a);
    append_esc(&mut text, t);
    append_esc(&mut text, v);
    proof { assert(text@ =~= esc(a@) + esc(t@) + esc(v@)); }
    Key::composite(0, text)
}

} // verus!
