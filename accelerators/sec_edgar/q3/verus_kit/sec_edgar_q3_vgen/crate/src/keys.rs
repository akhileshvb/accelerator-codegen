// Join keys of text columns as Index keys: each column escaped (so no key is a prefix of another),
// then concatenated.
use vstd::prelude::*;
use verus_prooflib::ordered::{Key, KeyView};

verus! {

pub open spec fn escape(bytes: Seq<u8>) -> Seq<u8>
    decreases bytes.len(),
{
    if bytes.len() == 0 { seq![0u8] }
    else { seq![1u8, bytes[0]] + escape(bytes.skip(1)) }
}

pub proof fn lemma_escape_end(bytes: Seq<u8>)
    ensures escape(bytes).len() > 0, escape(bytes).last() == 0u8,
        escape(bytes) == escape(bytes).drop_last().push(0u8),
    decreases bytes.len(),
{
    if bytes.len() > 0 {
        lemma_escape_end(bytes.skip(1));
    }
    assert(escape(bytes) =~= escape(bytes).drop_last().push(0u8));
}

pub proof fn lemma_escape_push(bytes: Seq<u8>, byte: u8)
    ensures escape(bytes.push(byte)) == escape(bytes).drop_last() + seq![1u8, byte, 0u8],
    decreases bytes.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(escape, 2);
    if bytes.len() > 0 {
        lemma_escape_end(bytes.skip(1));
        lemma_escape_push(bytes.skip(1), byte);
        assert(bytes.push(byte).skip(1) =~= bytes.skip(1).push(byte));
        assert(escape(bytes).len() > 0);
        assert((seq![1u8, bytes[0]] + escape(bytes.skip(1))).drop_last()
            =~= seq![1u8, bytes[0]] + escape(bytes.skip(1)).drop_last());
        assert((seq![1u8, bytes[0]] + escape(bytes.skip(1)).drop_last()) + seq![1u8, byte, 0u8]
            =~= seq![1u8, bytes[0]] + (escape(bytes.skip(1)).drop_last() + seq![1u8, byte, 0u8]));
    }
}

/// An escaped column ends where its first 0 byte is: what follows is another column.
pub proof fn lemma_escape_split(a: Seq<u8>, x: Seq<u8>, b: Seq<u8>, y: Seq<u8>)
    requires escape(a) + x == escape(b) + y,
    ensures a == b, x == y,
    decreases a.len() + b.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(escape, 2);
    let (l, r) = (escape(a) + x, escape(b) + y);
    if a.len() == 0 || b.len() == 0 {
        assert(l[0] == (if a.len() == 0 { 0u8 } else { 1u8 }));
        assert(r[0] == (if b.len() == 0 { 0u8 } else { 1u8 }));
        assert(a.len() == 0 && b.len() == 0);
        assert(a =~= b);
        assert(escape(a) =~= seq![0u8] && escape(b) =~= seq![0u8]);
        assert(l.skip(1) =~= x && r.skip(1) =~= y);
        assert(x =~= y);
    } else {
        assert(l[1] == a[0] && r[1] == b[0]);
        assert(a[0] == b[0]);
        assert(l.skip(2) =~= escape(a.skip(1)) + x);
        assert(r.skip(2) =~= escape(b.skip(1)) + y);
        lemma_escape_split(a.skip(1), x, b.skip(1), y);
        assert(a =~= seq![a[0]] + a.skip(1));
        assert(b =~= seq![b[0]] + b.skip(1));
    }
}

pub open spec fn key1(a: Seq<u8>) -> KeyView {
    (0int, escape(a))
}

pub open spec fn key2(a: Seq<u8>, b: Seq<u8>) -> KeyView {
    (0int, escape(a) + escape(b))
}

pub open spec fn key3(a: Seq<u8>, b: Seq<u8>, c: Seq<u8>) -> KeyView {
    (0int, escape(a) + escape(b) + escape(c))
}

pub proof fn lemma_key2_inj(a: Seq<u8>, b: Seq<u8>, a2: Seq<u8>, b2: Seq<u8>)
    requires key2(a, b) == key2(a2, b2),
    ensures a == a2, b == b2,
{
    lemma_escape_split(a, escape(b), a2, escape(b2));
    lemma_escape_split(b, Seq::empty(), b2, Seq::empty());
    assert(escape(b) + Seq::<u8>::empty() =~= escape(b));
}

pub proof fn lemma_key3_inj(a: Seq<u8>, b: Seq<u8>, c: Seq<u8>, a2: Seq<u8>, b2: Seq<u8>, c2: Seq<u8>)
    requires key3(a, b, c) == key3(a2, b2, c2),
    ensures a == a2, b == b2, c == c2,
{
    assert(escape(a) + escape(b) + escape(c) =~= escape(a) + (escape(b) + escape(c)));
    assert(escape(a2) + escape(b2) + escape(c2) =~= escape(a2) + (escape(b2) + escape(c2)));
    lemma_escape_split(a, escape(b) + escape(c), a2, escape(b2) + escape(c2));
    lemma_escape_split(b, escape(c), b2, escape(c2));
    lemma_escape_split(c, Seq::empty(), c2, Seq::empty());
    assert(escape(c) + Seq::<u8>::empty() =~= escape(c));
}

pub fn append_escaped(out: &mut Vec<u8>, bytes: &[u8])
    ensures final(out)@ == old(out)@ + escape(bytes@),
{
    let ghost before = out@;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(),
            out@ == before + escape(bytes@.take(index as int)).drop_last(),
        decreases bytes.len() - index,
    {
        proof {
            lemma_escape_push(bytes@.take(index as int), bytes@[index as int]);
            assert(bytes@.take(index as int + 1) =~= bytes@.take(index as int).push(bytes@[index as int]));
        }
        out.push(1);
        out.push(bytes[index]);
        proof {
            assert(out@ =~= before + escape(bytes@.take(index as int + 1)).drop_last());
        }
        index += 1;
    }
    proof {
        assert(bytes@.take(index as int) =~= bytes@);
        lemma_escape_end(bytes@);
    }
    out.push(0);
    proof {
        assert(out@ == (before + escape(bytes@).drop_last()).push(0u8));
        assert(out@ =~= before + escape(bytes@));
    }
}

pub fn make_key2(a: &[u8], b: &[u8]) -> (k: Key)
    ensures k.view() == key2(a@, b@),
{
    let mut t: Vec<u8> = Vec::new();
    append_escaped(&mut t, a);
    append_escaped(&mut t, b);
    proof { assert(t@ =~= Seq::<u8>::empty() + escape(a@) + escape(b@)); }
    Key::text(t)
}

pub fn make_key3(a: &[u8], b: &[u8], c: &[u8]) -> (k: Key)
    ensures k.view() == key3(a@, b@, c@),
{
    let mut t: Vec<u8> = Vec::new();
    append_escaped(&mut t, a);
    append_escaped(&mut t, b);
    append_escaped(&mut t, c);
    proof { assert(t@ =~= Seq::<u8>::empty() + escape(a@) + escape(b@) + escape(c@)); }
    Key::text(t)
}

pub fn make_key1(a: &[u8]) -> (k: Key)
    ensures k.view() == key1(a@),
{
    let mut t: Vec<u8> = Vec::new();
    append_escaped(&mut t, a);
    proof { assert(t@ =~= Seq::<u8>::empty() + escape(a@)); }
    Key::text(t)
}

}
