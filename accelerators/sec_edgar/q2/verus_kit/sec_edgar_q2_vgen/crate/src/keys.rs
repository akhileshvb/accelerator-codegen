// Sorting num rows by (adsh, tag, uom, value descending): which rows of a group hold its maximum.
use vstd::prelude::*;
use verus_common::bytes::*;

verus! {

pub struct GK {
    pub adsh: Seq<u8>,
    pub tag: Seq<u8>,
    pub uom: Seq<u8>,
    pub dk: int,
}

pub open spec fn same3(a: GK, b: GK) -> bool {
    a.adsh == b.adsh && a.tag == b.tag && a.uom == b.uom
}

pub open spec fn lt3(a: GK, b: GK) -> bool {
    bytes_lt(a.adsh, b.adsh)
        || (a.adsh == b.adsh && (bytes_lt(a.tag, b.tag) || (a.tag == b.tag && bytes_lt(a.uom, b.uom))))
}

pub open spec fn lt4(a: GK, b: GK) -> bool {
    lt3(a, b) || (same3(a, b) && a.dk < b.dk)
}

pub open spec fn le4(a: GK, b: GK) -> bool {
    !lt4(b, a)
}

pub proof fn lemma_lt3_irrefl(a: GK)
    ensures !lt3(a, a),
{
    lemma_bytes_lt_irrefl(a.adsh);
    lemma_bytes_lt_irrefl(a.tag);
    lemma_bytes_lt_irrefl(a.uom);
}

pub proof fn lemma_lt3_trans(a: GK, b: GK, c: GK)
    requires lt3(a, b), lt3(b, c),
    ensures lt3(a, c),
{
    if bytes_lt(a.adsh, b.adsh) && bytes_lt(b.adsh, c.adsh) { lemma_bytes_lt_trans(a.adsh, b.adsh, c.adsh); }
    if bytes_lt(a.adsh, b.adsh) && c.adsh == b.adsh {}
    if a.adsh == b.adsh && bytes_lt(b.adsh, c.adsh) {}
    if a.adsh == b.adsh && b.adsh == c.adsh {
        if bytes_lt(a.tag, b.tag) && bytes_lt(b.tag, c.tag) { lemma_bytes_lt_trans(a.tag, b.tag, c.tag); }
        if a.tag == b.tag && b.tag == c.tag && bytes_lt(a.uom, b.uom) && bytes_lt(b.uom, c.uom) { lemma_bytes_lt_trans(a.uom, b.uom, c.uom); }
    }
}

/// Of two keys, one is smaller or they agree on (adsh, tag, uom).
pub proof fn lemma_lt3_total(a: GK, b: GK)
    ensures lt3(a, b) || lt3(b, a) || same3(a, b),
{
    if a.adsh != b.adsh { lemma_bytes_lt_total(a.adsh, b.adsh); }
    else if a.tag != b.tag { lemma_bytes_lt_total(a.tag, b.tag); }
    else if a.uom != b.uom { lemma_bytes_lt_total(a.uom, b.uom); }
}

pub proof fn lemma_lt3_asym(a: GK, b: GK)
    ensures !(lt3(a, b) && lt3(b, a)),
{
    lemma_bytes_lt_asym(a.adsh, b.adsh);
    lemma_bytes_lt_asym(a.tag, b.tag);
    lemma_bytes_lt_asym(a.uom, b.uom);
    if a.adsh == b.adsh && bytes_lt(a.tag, b.tag) {}
}

pub proof fn lemma_same3_not_lt3(a: GK, b: GK)
    requires same3(a, b),
    ensures !lt3(a, b), !lt3(b, a),
{
    lemma_bytes_lt_irrefl(a.adsh);
    lemma_bytes_lt_irrefl(a.tag);
    lemma_bytes_lt_irrefl(a.uom);
}

/// le4 orders any two keys and is transitive.
pub proof fn lemma_le4_total(a: GK, b: GK)
    ensures le4(a, b) || le4(b, a),
{
    lemma_lt3_asym(a, b);
    if same3(a, b) {
        lemma_same3_not_lt3(a, b);
    }
}

pub proof fn lemma_le4_trans(a: GK, b: GK, c: GK)
    requires le4(a, b), le4(b, c),
    ensures le4(a, c),
{
    // not lt4(b, a), not lt4(c, b)  ==>  not lt4(c, a)
    if lt4(c, a) {
        lemma_lt3_total(a, b);
        lemma_lt3_total(b, c);
        lemma_lt3_total(c, a);
        if lt3(c, a) {
            // a <3 b or same3(a,b); b <3 c or same3(b,c); with c <3 a
            if lt3(a, b) { lemma_lt3_trans(c, a, b); }
            if same3(a, b) && lt3(b, c) { lemma_lt3_trans(c, a, b); }
            if same3(a, b) && same3(b, c) {
                lemma_same3_not_lt3(a, c);
            }
            if same3(a, b) && !lt3(b, c) && !same3(b, c) {}
        }
        if !lt3(c, a) {
            // same3(c, a) and c.dk < a.dk
            if lt3(a, b) { lemma_same3_not_lt3(c, a); lemma_lt3_trans(c, a, b); }
            if lt3(b, a) {}
            if lt3(b, c) {}
            if lt3(c, b) {}
            if same3(a, b) && same3(b, c) {
                assert(lt4(c, b) || lt4(b, a)) by {
                    if !(c.dk < b.dk) { assert(b.dk <= c.dk); }
                }
            }
        }
    }
}

pub open spec fn sorted4(ks: Seq<GK>) -> bool {
    forall|p: int, q: int| 0 <= p < q < ks.len() ==> le4(#[trigger] ks[p], #[trigger] ks[q])
}

/// The group's keys at positions of ks are the maximum: no key of the same group has a larger value (smaller dk).
pub open spec fn grp_top(ks: Seq<GK>, p: int) -> bool {
    forall|q: int| 0 <= q < ks.len() && same3(ks[q], ks[p]) ==> (#[trigger] ks[q]).dk >= ks[p].dk
}

pub proof fn lemma_grp_top_rule(ks: Seq<GK>, p: int)
    requires
        sorted4(ks),
        0 <= p < ks.len(),
    ensures
        grp_top(ks, p) == (p == 0 || !same3(ks[p - 1], ks[p]) || (ks[p - 1].dk == ks[p].dk && grp_top(ks, p - 1))),
{
    // for q > p in the same group, dk(p) <= dk(q); for q < p, dk(q) <= dk(p)
    assert forall|q: int| p < q < ks.len() && same3(ks[q], ks[p]) implies ks[p].dk <= (#[trigger] ks[q]).dk by {
        assert(le4(ks[p], ks[q]));
        lemma_same3_not_lt3(ks[p], ks[q]);
        if ks[q].dk < ks[p].dk { assert(lt4(ks[q], ks[p])); }
    }
    assert forall|q: int| 0 <= q < p && same3(ks[q], ks[p]) implies (#[trigger] ks[q]).dk <= ks[p].dk by {
        assert(le4(ks[q], ks[p]));
        lemma_same3_not_lt3(ks[p], ks[q]);
        if ks[p].dk < ks[q].dk { assert(lt4(ks[p], ks[q])); }
    }
    if p == 0 {
        assert(grp_top(ks, 0));
    } else if !same3(ks[p - 1], ks[p]) {
        // no earlier position is in the group: it would sit between p-1 and p
        assert forall|q: int| 0 <= q < p implies !same3(ks[q], ks[p]) by {
            if same3(ks[q], ks[p]) {
                if q == p - 1 {} else {
                    assert(le4(ks[q], ks[p - 1]));
                    assert(le4(ks[p - 1], ks[p]));
                    lemma_lt3_total(ks[p - 1], ks[p]);
                    lemma_lt3_total(ks[q], ks[p - 1]);
                    lemma_same3_not_lt3(ks[q], ks[p]);
                    // ks[q] <=3 ks[p-1] <=3 ks[p] = ks[q] (as 3-keys)
                    if lt3(ks[p - 1], ks[p]) { assert(lt3(ks[p - 1], ks[q])) by { assert(same3(ks[q], ks[p])); } }
                    if lt3(ks[p - 1], ks[q]) { assert(lt4(ks[p - 1], ks[q])); }
                    assert(same3(ks[p - 1], ks[p]));
                }
            }
        }
        assert(grp_top(ks, p));
    } else {
        if grp_top(ks, p) {
            assert(ks[p - 1].dk >= ks[p].dk);
            assert(ks[p - 1].dk == ks[p].dk);
            assert forall|q: int| 0 <= q < ks.len() && same3(ks[q], ks[p - 1]) implies (#[trigger] ks[q]).dk >= ks[p - 1].dk by {
                assert(same3(ks[q], ks[p]));
            }
        }
        if ks[p - 1].dk == ks[p].dk && grp_top(ks, p - 1) {
            assert forall|q: int| 0 <= q < ks.len() && same3(ks[q], ks[p]) implies (#[trigger] ks[q]).dk >= ks[p].dk by {
                assert(same3(ks[q], ks[p - 1]));
            }
        }
    }
}

} // verus!
