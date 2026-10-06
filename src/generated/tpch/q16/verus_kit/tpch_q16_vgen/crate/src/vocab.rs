use vstd::prelude::*;
use verus_common::bytes::*;

verus! {

// The TPC-H dbgen vocabulary for p_type (3 syllables, 150 strings) and
// p_brand (25 strings).  A string outside the vocabulary has code 65535 and
// is handled by the slow (string) path everywhere, so exactness never
// depends on the data following dbgen.

pub open spec fn word1(k: int) -> Seq<u8> {
    if k == 0 { seq![b'E', b'C', b'O', b'N', b'O', b'M', b'Y'] }
    else if k == 1 { seq![b'L', b'A', b'R', b'G', b'E'] }
    else if k == 2 { seq![b'M', b'E', b'D', b'I', b'U', b'M'] }
    else if k == 3 { seq![b'P', b'R', b'O', b'M', b'O'] }
    else if k == 4 { seq![b'S', b'M', b'A', b'L', b'L'] }
    else { seq![b'S', b'T', b'A', b'N', b'D', b'A', b'R', b'D'] }
}

pub open spec fn word2(k: int) -> Seq<u8> {
    if k == 0 { seq![b'A', b'N', b'O', b'D', b'I', b'Z', b'E', b'D'] }
    else if k == 1 { seq![b'B', b'R', b'U', b'S', b'H', b'E', b'D'] }
    else if k == 2 { seq![b'B', b'U', b'R', b'N', b'I', b'S', b'H', b'E', b'D'] }
    else if k == 3 { seq![b'P', b'L', b'A', b'T', b'E', b'D'] }
    else { seq![b'P', b'O', b'L', b'I', b'S', b'H', b'E', b'D'] }
}

pub open spec fn word3(k: int) -> Seq<u8> {
    if k == 0 { seq![b'B', b'R', b'A', b'S', b'S'] }
    else if k == 1 { seq![b'C', b'O', b'P', b'P', b'E', b'R'] }
    else if k == 2 { seq![b'N', b'I', b'C', b'K', b'E', b'L'] }
    else if k == 3 { seq![b'S', b'T', b'E', b'E', b'L'] }
    else { seq![b'T', b'I', b'N'] }
}

pub open spec fn vocab_type(i: int) -> Seq<u8> {
    word1(i / 25) + seq![32u8] + word2((i / 5) % 5) + seq![32u8] + word3(i % 5)
}

pub open spec fn vocab_brand(i: int) -> Seq<u8> {
    seq![b'B', b'r', b'a', b'n', b'd', b'#', (49 + i / 5) as u8, (49 + i % 5) as u8]
}

pub closed spec fn type_code_from(s: Seq<u8>, i: int) -> int
    decreases 150 - i,
{
    if i >= 150 { 65535 } else if vocab_type(i) == s { i } else { type_code_from(s, i + 1) }
}

pub closed spec fn brand_code_from(s: Seq<u8>, i: int) -> int
    decreases 25 - i,
{
    if i >= 25 { 65535 } else if vocab_brand(i) == s { i } else { brand_code_from(s, i + 1) }
}

pub closed spec fn type_code(s: Seq<u8>) -> int { type_code_from(s, 0) }

pub closed spec fn brand_code(s: Seq<u8>) -> int { brand_code_from(s, 0) }

pub proof fn lemma_type_code(s: Seq<u8>, i: int)
    requires 0 <= i <= 150,
    ensures type_code_from(s, i) == 65535 || (i <= type_code_from(s, i) < 150 && vocab_type(type_code_from(s, i)) == s),
        type_code_from(s, i) == 65535 ==> forall|j: int| i <= j < 150 ==> vocab_type(j) != s,
    decreases 150 - i,
{
    if i < 150 && vocab_type(i) != s { lemma_type_code(s, i + 1); }
}

pub proof fn lemma_brand_code(s: Seq<u8>, i: int)
    requires 0 <= i <= 25,
    ensures brand_code_from(s, i) == 65535 || (i <= brand_code_from(s, i) < 25 && vocab_brand(brand_code_from(s, i)) == s),
        brand_code_from(s, i) == 65535 ==> forall|j: int| i <= j < 25 ==> vocab_brand(j) != s,
    decreases 25 - i,
{
    if i < 25 && vocab_brand(i) != s { lemma_brand_code(s, i + 1); }
}

pub fn word1_exec(k: usize) -> (out: Vec<u8>)
    requires k < 6,
    ensures out@ == word1(k as int),
{
    if k == 0 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'E');
        v.push(b'C');
        v.push(b'O');
        v.push(b'N');
        v.push(b'O');
        v.push(b'M');
        v.push(b'Y');
        v
    }
    else if k == 1 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'L');
        v.push(b'A');
        v.push(b'R');
        v.push(b'G');
        v.push(b'E');
        v
    }
    else if k == 2 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'M');
        v.push(b'E');
        v.push(b'D');
        v.push(b'I');
        v.push(b'U');
        v.push(b'M');
        v
    }
    else if k == 3 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'P');
        v.push(b'R');
        v.push(b'O');
        v.push(b'M');
        v.push(b'O');
        v
    }
    else if k == 4 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'S');
        v.push(b'M');
        v.push(b'A');
        v.push(b'L');
        v.push(b'L');
        v
    }
    else {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'S');
        v.push(b'T');
        v.push(b'A');
        v.push(b'N');
        v.push(b'D');
        v.push(b'A');
        v.push(b'R');
        v.push(b'D');
        v
    }
}

pub fn word2_exec(k: usize) -> (out: Vec<u8>)
    requires k < 5,
    ensures out@ == word2(k as int),
{
    if k == 0 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'A');
        v.push(b'N');
        v.push(b'O');
        v.push(b'D');
        v.push(b'I');
        v.push(b'Z');
        v.push(b'E');
        v.push(b'D');
        v
    }
    else if k == 1 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'B');
        v.push(b'R');
        v.push(b'U');
        v.push(b'S');
        v.push(b'H');
        v.push(b'E');
        v.push(b'D');
        v
    }
    else if k == 2 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'B');
        v.push(b'U');
        v.push(b'R');
        v.push(b'N');
        v.push(b'I');
        v.push(b'S');
        v.push(b'H');
        v.push(b'E');
        v.push(b'D');
        v
    }
    else if k == 3 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'P');
        v.push(b'L');
        v.push(b'A');
        v.push(b'T');
        v.push(b'E');
        v.push(b'D');
        v
    }
    else {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'P');
        v.push(b'O');
        v.push(b'L');
        v.push(b'I');
        v.push(b'S');
        v.push(b'H');
        v.push(b'E');
        v.push(b'D');
        v
    }
}

pub fn word3_exec(k: usize) -> (out: Vec<u8>)
    requires k < 5,
    ensures out@ == word3(k as int),
{
    if k == 0 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'B');
        v.push(b'R');
        v.push(b'A');
        v.push(b'S');
        v.push(b'S');
        v
    }
    else if k == 1 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'C');
        v.push(b'O');
        v.push(b'P');
        v.push(b'P');
        v.push(b'E');
        v.push(b'R');
        v
    }
    else if k == 2 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'N');
        v.push(b'I');
        v.push(b'C');
        v.push(b'K');
        v.push(b'E');
        v.push(b'L');
        v
    }
    else if k == 3 {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'S');
        v.push(b'T');
        v.push(b'E');
        v.push(b'E');
        v.push(b'L');
        v
    }
    else {
        let mut v: Vec<u8> = Vec::new();
        v.push(b'T');
        v.push(b'I');
        v.push(b'N');
        v
    }
}

pub struct Vocab {
    pub types: Vec<Vec<u8>>,
    pub brands: Vec<Vec<u8>>,
}

impl Vocab {
    pub open spec fn ok(&self) -> bool {
        self.types@.len() == 150 && self.brands@.len() == 25
        && (forall|i: int| 0 <= i < 150 ==> (#[trigger] self.types@[i])@ == vocab_type(i))
        && (forall|i: int| 0 <= i < 25 ==> (#[trigger] self.brands@[i])@ == vocab_brand(i))
    }
}

pub fn append_bytes(out: &mut Vec<u8>, bytes: &[u8])
    ensures final(out)@ == old(out)@ + bytes@,
{
    let ghost before = out@;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), out@ == before + bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        out.push(bytes[index]);
        proof { assert(bytes@.take(index as int + 1) =~= bytes@.take(index as int).push(bytes@[index as int])); }
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
}

pub fn new_vocab() -> (v: Vocab)
    ensures v.ok(),
{
    let mut types: Vec<Vec<u8>> = Vec::new();
    while types.len() < 150
        invariant types.len() <= 150,
            forall|j: int| 0 <= j < types@.len() ==> (#[trigger] types@[j])@ == vocab_type(j),
        decreases 150 - types.len(),
    {
        let i = types.len();
        let mut s: Vec<u8> = Vec::new();
        let a = word1_exec(i / 25);
        append_bytes(&mut s, a.as_slice());
        s.push(32u8);
        let b = word2_exec((i / 5) % 5);
        append_bytes(&mut s, b.as_slice());
        s.push(32u8);
        let c = word3_exec(i % 5);
        append_bytes(&mut s, c.as_slice());
        proof { assert(s@ =~= vocab_type(i as int)); }
        types.push(s);
    }
    let mut brands: Vec<Vec<u8>> = Vec::new();
    while brands.len() < 25
        invariant brands.len() <= 25,
            forall|j: int| 0 <= j < brands@.len() ==> (#[trigger] brands@[j])@ == vocab_brand(j),
        decreases 25 - brands.len(),
    {
        let i = brands.len();
        let mut s: Vec<u8> = Vec::new();
        s.push(b'B'); s.push(b'r'); s.push(b'a'); s.push(b'n'); s.push(b'd'); s.push(b'#');
        s.push((49 + i / 5) as u8);
        s.push((49 + i % 5) as u8);
        proof { assert(s@ =~= vocab_brand(i as int)); }
        brands.push(s);
    }
    Vocab { types, brands }
}

/// The code of a type string: its vocabulary index, or 65535.
pub fn type_code_exec(vocab: &Vocab, s: &[u8]) -> (code: u16)
    requires vocab.ok(),
    ensures code as int == type_code(s@),
{
    let mut i: usize = 0;
    while i < 150
        invariant i <= 150, vocab.ok(),
            forall|j: int| 0 <= j < i ==> vocab_type(j) != s@,
        decreases 150 - i,
    {
        if verus_common::bytes::bytes_eq_exec(vocab.types[i].as_slice(), s) {
            proof { reveal_with_fuel(type_code_from, 1); lemma_type_code_prefix(s@, i as int); }
            return i as u16;
        }
        i += 1;
    }
    proof { lemma_type_code_none(s@, 0); }
    65535u16
}

pub fn brand_code_exec(vocab: &Vocab, s: &[u8]) -> (code: u16)
    requires vocab.ok(),
    ensures code as int == brand_code(s@),
{
    let mut i: usize = 0;
    while i < 25
        invariant i <= 25, vocab.ok(),
            forall|j: int| 0 <= j < i ==> vocab_brand(j) != s@,
        decreases 25 - i,
    {
        if verus_common::bytes::bytes_eq_exec(vocab.brands[i].as_slice(), s) {
            proof { lemma_brand_code_prefix(s@, i as int); }
            return i as u16;
        }
        i += 1;
    }
    proof { lemma_brand_code_none(s@, 0); }
    65535u16
}

pub proof fn lemma_type_code_prefix(s: Seq<u8>, i: int)
    requires 0 <= i < 150, vocab_type(i) == s, forall|j: int| 0 <= j < i ==> vocab_type(j) != s,
    ensures type_code(s) == i,
{
    lemma_type_code_skip(s, i);
}

pub proof fn lemma_type_code_skip(s: Seq<u8>, i: int)
    requires 0 <= i < 150, vocab_type(i) == s, forall|j: int| 0 <= j < i ==> vocab_type(j) != s,
    ensures type_code_from(s, 0) == i,
{
    lemma_type_code_walk(s, i, 0);
}

pub proof fn lemma_type_code_walk(s: Seq<u8>, i: int, k: int)
    requires 0 <= k <= i < 150, vocab_type(i) == s, forall|j: int| k <= j < i ==> vocab_type(j) != s,
    ensures type_code_from(s, k) == i,
    decreases i - k,
{
    if k < i { lemma_type_code_walk(s, i, k + 1); }
}

pub proof fn lemma_type_code_none(s: Seq<u8>, k: int)
    requires 0 <= k <= 150, forall|j: int| k <= j < 150 ==> vocab_type(j) != s,
    ensures type_code_from(s, k) == 65535,
    decreases 150 - k,
{
    if k < 150 { lemma_type_code_none(s, k + 1); }
}

pub proof fn lemma_brand_code_prefix(s: Seq<u8>, i: int)
    requires 0 <= i < 25, vocab_brand(i) == s, forall|j: int| 0 <= j < i ==> vocab_brand(j) != s,
    ensures brand_code(s) == i,
{
    lemma_brand_code_walk(s, i, 0);
}

pub proof fn lemma_brand_code_walk(s: Seq<u8>, i: int, k: int)
    requires 0 <= k <= i < 25, vocab_brand(i) == s, forall|j: int| k <= j < i ==> vocab_brand(j) != s,
    ensures brand_code_from(s, k) == i,
    decreases i - k,
{
    if k < i { lemma_brand_code_walk(s, i, k + 1); }
}

pub proof fn lemma_brand_code_none(s: Seq<u8>, k: int)
    requires 0 <= k <= 25, forall|j: int| k <= j < 25 ==> vocab_brand(j) != s,
    ensures brand_code_from(s, k) == 65535,
    decreases 25 - k,
{
    if k < 25 { lemma_brand_code_none(s, k + 1); }
}

pub proof fn lemma_type_code_sound(s: Seq<u8>)
    ensures type_code(s) == 65535 || (0 <= type_code(s) < 150 && vocab_type(type_code(s)) == s),
{
    lemma_type_code(s, 0);
}

pub proof fn lemma_brand_code_sound(s: Seq<u8>)
    ensures brand_code(s) == 65535 || (0 <= brand_code(s) < 25 && vocab_brand(brand_code(s)) == s),
{
    lemma_brand_code(s, 0);
}

pub proof fn lemma_lt_common_prefix(prefix: Seq<u8>, a: Seq<u8>, b: Seq<u8>)
    ensures bytes_lt(prefix + a, prefix + b) == bytes_lt(a, b),
    decreases prefix.len(),
{
    if prefix.len() > 0 {
        let rest = prefix.drop_first();
        assert((prefix + a).drop_first() =~= rest + a);
        assert((prefix + b).drop_first() =~= rest + b);
        assert((prefix + a)[0] == prefix[0]);
        assert((prefix + b)[0] == prefix[0]);
        lemma_lt_common_prefix(rest, a, b);
    }
}

pub proof fn lemma_brand_order(i: int, j: int)
    requires 0 <= i < j < 25,
    ensures bytes_lt(vocab_brand(i), vocab_brand(j)),
{
    let prefix = seq![b'B', b'r', b'a', b'n', b'd', b'#'];
    let a = seq![(49 + i / 5) as u8, (49 + i % 5) as u8];
    let b = seq![(49 + j / 5) as u8, (49 + j % 5) as u8];
    assert(vocab_brand(i) =~= prefix + a);
    assert(vocab_brand(j) =~= prefix + b);
    lemma_lt_common_prefix(prefix, a, b);
    reveal_with_fuel(bytes_lt, 3);
    assert(a.drop_first() =~= seq![a[1]]);
    assert(b.drop_first() =~= seq![b[1]]);
    assert(a.drop_first().drop_first() =~= Seq::<u8>::empty());
    assert(b.drop_first().drop_first() =~= Seq::<u8>::empty());
}

}
