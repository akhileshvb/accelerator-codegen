// Adding one transaction's per-group contributions to the state's groups.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView, lemma_ordered_unique};
use verus_prooflib::wide::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::model::*;

verus! {

pub open spec fn gk_at(m: GMap, k: GK) -> Option<Grp> {
    gm_at(m, okey(k.y, k.uom), ikey(k.tag, k.ver))
}

pub open spec fn gv_c(m: GMap, k: GK) -> int {
    match gk_at(m, k) { Some(g) => g.cnt@, None => 0 }
}

pub open spec fn gv_s(m: GMap, k: GK) -> int {
    match gk_at(m, k) { Some(g) => g.sum@, None => 0 }
}

/// Every stored group has a positive count.
pub open spec fn pos_ok(m: GMap) -> bool {
    forall|ok: KeyView, ik: KeyView| #![trigger gm_at(m, ok, ik)] gm_at(m, ok, ik) is Some ==> gm_at(m, ok, ik)->0.cnt@ > 0
}

pub proof fn lemma_key_split(k: GK, k2: GK)
    requires okey(k.y, k.uom) == okey(k2.y, k2.uom), ikey(k.tag, k.ver) == ikey(k2.tag, k2.ver),
    ensures k == k2,
{
    lemma_okey_inj(k.y, k.uom, k2.y, k2.uom);
    lemma_ikey_inj(k.tag, k.ver, k2.tag, k2.ver);
}

/// Adds the entry's counts to its group, dropping the group when its count reaches zero.
pub fn gm_apply(groups: &mut GMap, e: &AccEnt)
    requires
        gm_ok(*old(groups)),
        pos_ok(*old(groups)),
        gv_c(*old(groups), e.gk()) + e.cnt@ >= 0,
        gv_c(*old(groups), e.gk()) + e.cnt@ == 0 ==> gv_s(*old(groups), e.gk()) + e.sum@ == 0,
    ensures
        gm_ok(*final(groups)),
        pos_ok(*final(groups)),
        forall|k: GK| #![trigger gv_c(*final(groups), k)] gv_c(*final(groups), k) == gv_c(*old(groups), k) + if k == e.gk() { e.cnt@ } else { 0 },
        forall|k: GK| #![trigger gv_s(*final(groups), k)] gv_s(*final(groups), k) == gv_s(*old(groups), k) + if k == e.gk() { e.sum@ } else { 0 },
{
    let ghost old_g = *groups;
    let ghost ke = e.gk();
    let found = gm_get(groups, e.y, e.uom.as_slice(), &e.tag, &e.ver);
    let (c, s) = match &found {
        Some(g) => (g.cnt.add(&e.cnt), g.sum.add(&e.sum)),
        None => (e.cnt.dup(), e.sum.dup()),
    };
    proof {
        assert(gk_at(old_g, ke) == match found { Some(g) => Some(*g), None => None });
        assert(c@ == gv_c(old_g, ke) + e.cnt@ && s@ == gv_s(old_g, ke) + e.sum@);
    }
    if c.is_zero() {
        gm_del(groups, e.y, e.uom.as_slice(), &e.tag, &e.ver);
        proof {
            assert forall|k: GK| #![trigger gv_c(*groups, k)] gv_c(*groups, k) == gv_c(old_g, k) + if k == ke { e.cnt@ } else { 0 } by {
                if k != ke {
                    if okey(k.y, k.uom) == okey(ke.y, ke.uom) && ikey(k.tag, k.ver) == ikey(ke.tag, ke.ver) { lemma_key_split(k, ke); }
                }
            }
            assert forall|k: GK| #![trigger gv_s(*groups, k)] gv_s(*groups, k) == gv_s(old_g, k) + if k == ke { e.sum@ } else { 0 } by {
                if k != ke {
                    if okey(k.y, k.uom) == okey(ke.y, ke.uom) && ikey(k.tag, k.ver) == ikey(ke.tag, ke.ver) { lemma_key_split(k, ke); }
                } else {
                    assert(gv_s(*groups, ke) == 0);
                    assert(c@ == 0);
                }
            }
            assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*groups, ok, ik)] gm_at(*groups, ok, ik) is Some implies gm_at(*groups, ok, ik)->0.cnt@ > 0 by {
                assert(gm_at(*groups, ok, ik) == gm_at(old_g, ok, ik));
            }
        }
    } else {
        let g = Grp { tag: copy_opt(&e.tag), ver: copy_opt(&e.ver), cnt: c, sum: s };
        proof { assert(g.ik() == ikey(ke.tag, ke.ver)); }
        let ghost cv = c@;
        gm_set(groups, e.y, e.uom.as_slice(), g);
        proof {
            assert forall|k: GK| #![trigger gv_c(*groups, k)] gv_c(*groups, k) == gv_c(old_g, k) + if k == ke { e.cnt@ } else { 0 } by {
                if k != ke {
                    if okey(k.y, k.uom) == okey(ke.y, ke.uom) && ikey(k.tag, k.ver) == ikey(ke.tag, ke.ver) { lemma_key_split(k, ke); }
                }
            }
            assert forall|k: GK| #![trigger gv_s(*groups, k)] gv_s(*groups, k) == gv_s(old_g, k) + if k == ke { e.sum@ } else { 0 } by {
                if k != ke {
                    if okey(k.y, k.uom) == okey(ke.y, ke.uom) && ikey(k.tag, k.ver) == ikey(ke.tag, ke.ver) { lemma_key_split(k, ke); }
                }
            }
            assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*groups, ok, ik)] gm_at(*groups, ok, ik) is Some implies gm_at(*groups, ok, ik)->0.cnt@ > 0 by {
                if ok == okey(ke.y, ke.uom) && ik == ikey(ke.tag, ke.ver) {
                    assert(gm_at(*groups, ok, ik)->0.cnt@ == cv);
                } else {
                    assert(gm_at(*groups, ok, ik) == gm_at(old_g, ok, ik));
                }
            }
        }
    }
}

pub open spec fn arc_val<T>(a: &Arc<T>) -> T {
    **a
}

pub open spec fn done_k(entries: Seq<(KeyView, AccEnt)>, i: int, k: GK) -> bool {
    exists|j: int| #![trigger entries[j]] 0 <= j < i && entries[j].0 == fkey(k)
}

/// Adds every entry of the contributions to the state's groups.
pub fn merge_acc(groups: &mut GMap, acc: &Index<AccEnt>)
    requires
        gm_ok(*old(groups)),
        pos_ok(*old(groups)),
        acc_ok(*acc),
        forall|k: GK| #![trigger gv_c(*old(groups), k)] gv_c(*old(groups), k) + acc_c(*acc, k) >= 0,
        forall|k: GK| #![trigger gv_c(*old(groups), k)] gv_c(*old(groups), k) + acc_c(*acc, k) == 0 ==> gv_s(*old(groups), k) + acc_s(*acc, k) == 0,
    ensures
        gm_ok(*final(groups)),
        pos_ok(*final(groups)),
        forall|k: GK| #![trigger gv_c(*final(groups), k)] gv_c(*final(groups), k) == gv_c(*old(groups), k) + acc_c(*acc, k),
        forall|k: GK| #![trigger gv_s(*final(groups), k)] gv_s(*final(groups), k) == gv_s(*old(groups), k) + acc_s(*acc, k),
{
    let ghost g0 = *groups;
    let rows = acc.rows();
    let ghost entries = acc.entries();
    proof {
        acc.map_model();
        lemma_ordered_unique(entries);
        assert(rows@.map_values(|row: (Key, Arc<AccEnt>)| (row.0.view(), *row.1)) == entries);
    }
    let mut i = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            entries == acc.entries(),
            rows@.map_values(|row: (Key, Arc<AccEnt>)| (row.0.view(), *row.1)) == entries,
            acc_ok(*acc), acc.wf(),
            forall|a: int, b: int| 0 <= a < b < entries.len() ==> (#[trigger] entries[a]).0 != (#[trigger] entries[b]).0,
            forall|row: (KeyView, AccEnt)| #![trigger entries.contains(row)] entries.contains(row) ==> acc.at(row.0) == Some(row.1),
            gm_ok(*groups), pos_ok(*groups),
            forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(*acc, k) >= 0,
            forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(*acc, k) == 0 ==> gv_s(g0, k) + acc_s(*acc, k) == 0,
            forall|k: GK| #![trigger gv_c(*groups, k)] gv_c(*groups, k) == gv_c(g0, k) + if done_k(entries, i as int, k) { acc_c(*acc, k) } else { 0 },
            forall|k: GK| #![trigger gv_s(*groups, k)] gv_s(*groups, k) == gv_s(g0, k) + if done_k(entries, i as int, k) { acc_s(*acc, k) } else { 0 },
        decreases rows.len() - i,
    {
        let ent = &rows[i].1;
        let ghost e = arc_val(ent);
        let ghost k = e.gk();
        let ghost before = *groups;
        proof {
            assert(entries[i as int] == (rows@[i as int].0.view(), e));
            assert(entries.contains(entries[i as int]));
            assert(acc.at(entries[i as int].0) == Some(e));
            assert(entries[i as int].0 == fkey(k));
            assert(!done_k(entries, i as int, k)) by {
                assert forall|j: int| 0 <= j < i as int implies (#[trigger] entries[j]).0 != fkey(k) by {
                    assert(entries[j].0 != entries[i as int].0);
                }
            }
            assert(acc_c(*acc, k) == e.cnt@ && acc_s(*acc, k) == e.sum@);
            assert(gv_c(*groups, k) == gv_c(g0, k));
            assert(gv_s(*groups, k) == gv_s(g0, k));
        }
        gm_apply(groups, ent);
        proof {
            assert forall|k2: GK| #![trigger gv_c(*groups, k2)]
                gv_c(*groups, k2) == gv_c(g0, k2) + if done_k(entries, i as int + 1, k2) { acc_c(*acc, k2) } else { 0 } by {
                if k2 == k {
                    assert(done_k(entries, i as int + 1, k2)) by { assert(entries[i as int].0 == fkey(k2)); }
                } else {
                    if fkey(k2) == fkey(k) { lemma_fkey_inj(k2, k); }
                    assert(done_k(entries, i as int + 1, k2) == done_k(entries, i as int, k2)) by {
                        if done_k(entries, i as int + 1, k2) {
                            let j = choose|j: int| 0 <= j < i as int + 1 && entries[j].0 == fkey(k2);
                            assert(j != i as int);
                        }
                    }
                }
            }
            assert forall|k2: GK| #![trigger gv_s(*groups, k2)]
                gv_s(*groups, k2) == gv_s(g0, k2) + if done_k(entries, i as int + 1, k2) { acc_s(*acc, k2) } else { 0 } by {
                if k2 == k {
                    assert(done_k(entries, i as int + 1, k2)) by { assert(entries[i as int].0 == fkey(k2)); }
                } else {
                    if fkey(k2) == fkey(k) { lemma_fkey_inj(k2, k); }
                    assert(done_k(entries, i as int + 1, k2) == done_k(entries, i as int, k2)) by {
                        if done_k(entries, i as int + 1, k2) {
                            let j = choose|j: int| 0 <= j < i as int + 1 && entries[j].0 == fkey(k2);
                            assert(j != i as int);
                        }
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|k: GK| #![trigger gv_c(*groups, k)] gv_c(*groups, k) == gv_c(g0, k) + acc_c(*acc, k) by {
            if !done_k(entries, i as int, k) {
                assert(acc.at(fkey(k)) is None) by {
                    if acc.at(fkey(k)) is Some {
                        let row = (fkey(k), acc.at(fkey(k))->0);
                        assert(entries.contains(row));
                        let j = choose|j: int| 0 <= j < entries.len() && entries[j] == row;
                        assert(done_k(entries, i as int, k));
                    }
                }
            }
        }
        assert forall|k: GK| #![trigger gv_s(*groups, k)] gv_s(*groups, k) == gv_s(g0, k) + acc_s(*acc, k) by {
            if !done_k(entries, i as int, k) {
                assert(acc.at(fkey(k)) is None) by {
                    if acc.at(fkey(k)) is Some {
                        let row = (fkey(k), acc.at(fkey(k))->0);
                        assert(entries.contains(row));
                        let j = choose|j: int| 0 <= j < entries.len() && entries[j] == row;
                        assert(done_k(entries, i as int, k));
                    }
                }
            }
        }
    }
}


} // verus!
