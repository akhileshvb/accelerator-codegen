// Byte strings: C-collation order (bytes_lt) is a strict total order, and
// the executable comparisons agree with it.
use vstd::prelude::*;

verus! {

/// Byte-wise lexicographic order (C collation): a < b.
pub open spec fn bytes_lt(a: Seq<u8>, b: Seq<u8>) -> bool
    decreases a.len(),
{
    if a.len() == 0 {
        b.len() > 0
    } else if b.len() == 0 {
        false
    } else if a[0] != b[0] {
        a[0] < b[0]
    } else {
        bytes_lt(a.drop_first(), b.drop_first())
    }
}


pub proof fn lemma_bytes_lt_irrefl(a: Seq<u8>)
    ensures
        !bytes_lt(a, a),
    decreases a.len(),
{
    if a.len() > 0 {
        lemma_bytes_lt_irrefl(a.drop_first());
    }
}

pub proof fn lemma_bytes_lt_asym(a: Seq<u8>, b: Seq<u8>)
    ensures
        !(bytes_lt(a, b) && bytes_lt(b, a)),
    decreases a.len(),
{
    if a.len() > 0 && b.len() > 0 && a[0] == b[0] {
        lemma_bytes_lt_asym(a.drop_first(), b.drop_first());
    }
}

pub proof fn lemma_bytes_lt_trans(a: Seq<u8>, b: Seq<u8>, c: Seq<u8>)
    requires
        bytes_lt(a, b),
        bytes_lt(b, c),
    ensures
        bytes_lt(a, c),
    decreases a.len(),
{
    if a.len() > 0 && b.len() > 0 && c.len() > 0 && a[0] == b[0] && b[0] == c[0] {
        lemma_bytes_lt_trans(a.drop_first(), b.drop_first(), c.drop_first());
    }
}

pub proof fn lemma_bytes_lt_total(a: Seq<u8>, b: Seq<u8>)
    requires
        a != b,
    ensures
        bytes_lt(a, b) || bytes_lt(b, a),
    decreases a.len(),
{
    if a.len() == 0 {
        assert(b.len() > 0) by {
            if b.len() == 0 {
                assert(a =~= b);
            }
        }
    } else if b.len() == 0 {
    } else if a[0] != b[0] {
    } else {
        if a.drop_first() == b.drop_first() {
            assert(a =~= seq![a[0]] + a.drop_first());
            assert(b =~= seq![b[0]] + b.drop_first());
        }
        lemma_bytes_lt_total(a.drop_first(), b.drop_first());
    }
}

proof fn lemma_bytes_lt_skip(a: Seq<u8>, b: Seq<u8>, i: int)
    requires
        0 <= i <= a.len(),
        i <= b.len(),
        a.take(i) == b.take(i),
    ensures
        bytes_lt(a, b) == bytes_lt(a.skip(i), b.skip(i)),
    decreases i,
{
    if i > 0 {
        assert(a[0] == a.take(i)[0]);
        assert(b[0] == b.take(i)[0]);
        assert(a.drop_first().take(i - 1) =~= a.take(i).drop_first());
        assert(b.drop_first().take(i - 1) =~= b.take(i).drop_first());
        lemma_bytes_lt_skip(a.drop_first(), b.drop_first(), i - 1);
        assert(a.drop_first().skip(i - 1) =~= a.skip(i));
        assert(b.drop_first().skip(i - 1) =~= b.skip(i));
    } else {
        assert(a.skip(0) =~= a);
        assert(b.skip(0) =~= b);
    }
}

pub fn bytes_lt_exec(a: &[u8], b: &[u8]) -> (r: bool)
    ensures
        r == bytes_lt(a@, b@),
{
    let n = if a.len() < b.len() {
        a.len()
    } else {
        b.len()
    };
    let mut i: usize = 0;
    while i < n
        invariant
            0 <= i <= n,
            n <= a@.len(),
            n <= b@.len(),
            n == a@.len() || n == b@.len(),
            a@.take(i as int) == b@.take(i as int),
        decreases n - i,
    {
        if a[i] != b[i] {
            proof {
                lemma_bytes_lt_skip(a@, b@, i as int);
                assert(a@.skip(i as int)[0] == a@[i as int]);
                assert(b@.skip(i as int)[0] == b@[i as int]);
            }
            return a[i] < b[i];
        }
        proof {
            assert(a@.take(i as int + 1) =~= a@.take(i as int).push(a@[i as int]));
            assert(b@.take(i as int + 1) =~= b@.take(i as int).push(b@[i as int]));
        }
        i += 1;
    }
    proof {
        lemma_bytes_lt_skip(a@, b@, n as int);
    }
    a.len() < b.len()
}

pub fn bytes_eq_exec(a: &[u8], b: &[u8]) -> (r: bool)
    ensures
        r == (a@ == b@),
{
    if a.len() != b.len() {
        return false;
    }
    let n = a.len();
    let mut i: usize = 0;
    while i < n
        invariant
            0 <= i <= n,
            n == a@.len(),
            n == b@.len(),
            forall|j: int| 0 <= j < i ==> a@[j] == b@[j],
        decreases n - i,
    {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    proof {
        assert(a@ =~= b@);
    }
    true
}

} // verus!
