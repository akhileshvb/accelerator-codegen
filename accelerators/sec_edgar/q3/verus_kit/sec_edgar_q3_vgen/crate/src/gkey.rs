// Index keys of groups: a (name, company) pair, injectively; a company alone is the pair with no name.
use vstd::prelude::*;
use verus_prooflib::ordered::{Key, KeyView};

verus! {

pub open spec fn opt_int(x: Option<i64>) -> Option<int> {
    match x { Some(a) => Some(a as int), None => None }
}

pub open spec fn opt_bytes(x: Option<Vec<u8>>) -> Option<Seq<u8>> {
    match x { Some(a) => Some(a@), None => None }
}

pub open spec fn flag<A>(x: Option<A>) -> u8 {
    if x is Some { 1u8 } else { 0u8 }
}

pub open spec fn gk(name: Option<Seq<u8>>, cik: Option<int>) -> KeyView {
    (match cik { Some(a) => a, None => 0int },
     seq![flag(cik), flag(name)] + match name { Some(b) => b, None => Seq::empty() })
}

pub proof fn lemma_gk_inj(name: Option<Seq<u8>>, cik: Option<int>, name2: Option<Seq<u8>>, cik2: Option<int>)
    requires gk(name, cik) == gk(name2, cik2),
    ensures name == name2, cik == cik2,
{
    let (a, b) = (gk(name, cik).1, gk(name2, cik2).1);
    assert(a[0] == flag(cik) && a[1] == flag(name));
    assert(b[0] == flag(cik2) && b[1] == flag(name2));
    if name is Some && name2 is Some {
        assert(a.skip(2) =~= name->0);
        assert(b.skip(2) =~= name2->0);
    }
}

pub fn gk_key(name: &Option<Vec<u8>>, cik: Option<i64>) -> (k: Key)
    ensures k.view() == gk(match name { Some(b) => Some(b@), None => None }, opt_int(cik)),
{
    let mut text: Vec<u8> = Vec::with_capacity(match name { Some(b) => if b.len() < 1048576 { b.len() + 2 } else { 2 }, None => 2 });
    text.push(if cik.is_some() { 1 } else { 0 });
    text.push(if name.is_some() { 1 } else { 0 });
    if let Some(b) = name {
        let mut i: usize = 0;
        while i < b.len()
            invariant
                i <= b.len(),
                text@ == seq![if cik.is_some() { 1u8 } else { 0u8 }, if name.is_some() { 1u8 } else { 0u8 }] + b@.take(i as int),
            decreases b.len() - i,
        {
            text.push(b[i]);
            proof { assert(text@ =~= seq![if cik.is_some() { 1u8 } else { 0u8 }, if name.is_some() { 1u8 } else { 0u8 }] + b@.take(i as int + 1)); }
            i += 1;
        }
        proof { assert(b@.take(b.len() as int) =~= b@); }
    }
    let number = match cik { Some(a) => a, None => 0 };
    Key::composite(number, text)
}

}
