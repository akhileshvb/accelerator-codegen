// Reading the answer off the rank index: the first 200 entries of the triple's index, in order.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, Cursor, key_lt, lemma_key_order};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::merge::*;
use crate::f64bits::*;
use crate::glue_proofs::*;
use crate::rep::*;
use crate::answer::*;
use crate::ordering::*;
use crate::kernel::*;
use crate::rank::*;
use crate::emit::{OutT, cnt_i64, lemma_not_fits};

verus! {

proof fn lemma_lt_prefix(p: Seq<u8>, x: Seq<u8>, y: Seq<u8>)
    ensures bytes_lt(p + x, p + y) == bytes_lt(x, y),
    decreases p.len(),
{
    if p.len() == 0 {
        assert(p + x =~= x);
        assert(p + y =~= y);
    } else {
        assert((p + x).drop_first() =~= p.drop_first() + x);
        assert((p + y).drop_first() =~= p.drop_first() + y);
        lemma_lt_prefix(p.drop_first(), x, y);
    }
}

/// The escaped encoding keeps the byte order of the strings, whatever follows.
proof fn lemma_esc_lt(a: Seq<u8>, b: Seq<u8>, r1: Seq<u8>, r2: Seq<u8>)
    requires bytes_lt(a, b),
    ensures bytes_lt(esc(a) + r1, esc(b) + r2),
    decreases a.len(),
{
    if a.len() == 0 {
        lemma_body_head(b);
        assert(esc(a) =~= seq![0u8]);
        assert((esc(a) + r1)[0] == 0u8);
        assert((esc(b) + r2)[0] == 1u8);
        reveal_with_fuel(bytes_lt, 2);
    } else {
        lemma_body_head(a);
        lemma_body_head(b);
        let (ha, hb) = (seq![1u8, a[0]], seq![1u8, b[0]]);
        assert(esc(a) + r1 =~= ha + (esc(a.skip(1)) + r1));
        assert(esc(b) + r2 =~= hb + (esc(b.skip(1)) + r2));
        reveal_with_fuel(bytes_lt, 3);
        if a[0] == b[0] {
            assert(a.drop_first() =~= a.skip(1));
            assert(b.drop_first() =~= b.skip(1));
            lemma_esc_lt(a.skip(1), b.skip(1), r1, r2);
            lemma_lt_prefix(ha, esc(a.skip(1)) + r1, esc(b.skip(1)) + r2);
        }
    }
}

proof fn lemma_esc_opt_lt(x: Option<Seq<u8>>, y: Option<Seq<u8>>, r1: Seq<u8>, r2: Seq<u8>)
    requires opt_lt(x, y),
    ensures bytes_lt(esc_opt(x) + r1, esc_opt(y) + r2),
{
    if y is None {
        let a = x->0;
        if a.len() > 0 { lemma_body_head(a); }
        assert(esc(a)[0] == 0u8 || esc(a)[0] == 1u8);
        assert((esc_opt(x) + r1)[0] == esc(a)[0]);
        assert((esc_opt(y) + r2)[0] == 2u8);
    } else {
        lemma_esc_lt(x->0, y->0, r1, r2);
    }
}

/// ORDER BY name, tag, plabel on group keys.
pub open spec fn lexi_lt(n1: Option<Seq<u8>>, t1: Seq<u8>, p1: Option<Seq<u8>>, n2: Option<Seq<u8>>, t2: Seq<u8>, p2: Option<Seq<u8>>) -> bool {
    opt_lt(n1, n2) || (n1 == n2 && (bytes_lt(t1, t2) || (t1 == t2 && opt_lt(p1, p2))))
}

/// The stored group key sorts like the ORDER BY columns.
proof fn lemma_ikey_lt(n1: Option<Seq<u8>>, t1: Seq<u8>, p1: Option<Seq<u8>>, n2: Option<Seq<u8>>, t2: Seq<u8>, p2: Option<Seq<u8>>)
    requires lexi_lt(n1, t1, p1, n2, t2, p2),
    ensures bytes_lt(ikey(n1, t1, p1).1, ikey(n2, t2, p2).1),
{
    let (r1, r2) = (esc(t1) + esc_opt(p1), esc(t2) + esc_opt(p2));
    assert(ikey(n1, t1, p1).1 =~= esc_opt(n1) + r1);
    assert(ikey(n2, t2, p2).1 =~= esc_opt(n2) + r2);
    if opt_lt(n1, n2) {
        lemma_esc_opt_lt(n1, n2, r1, r2);
    } else if bytes_lt(t1, t2) {
        lemma_lt_prefix(esc_opt(n1), r1, r2);
        lemma_esc_lt(t1, t2, esc_opt(p1), esc_opt(p2));
    } else {
        let e = esc_opt(n1) + esc(t1);
        assert(ikey(n1, t1, p1).1 =~= e + esc_opt(p1));
        assert(ikey(n2, t2, p2).1 =~= e + esc_opt(p2));
        lemma_esc_opt_lt(p1, p2, Seq::empty(), Seq::empty());
        assert(esc_opt(p1) + Seq::<u8>::empty() =~= esc_opt(p1));
        assert(esc_opt(p2) + Seq::<u8>::empty() =~= esc_opt(p2));
        lemma_lt_prefix(e, esc_opt(p1), esc_opt(p2));
    }
}

/// The answer row of a stored group.
pub open spec fn g_row(p2: Seq<u8>, g: Grp) -> OutRow {
    OutRow { name: ov(g.name), stmt: Some(p2), tag: Some(g.tag@), plabel: ov(g.plabel), total_value: Some(f64_val(g.bits)), cnt: g.cnt@ }
}

pub open spec fn g_rows(p2: Seq<u8>, entries: Seq<(KeyView, Grp)>) -> Seq<OutRow> {
    entries.map_values(|e: (KeyView, Grp)| g_row(p2, e.1))
}

pub open spec fn triple_entries(rank: GMap, p: Params) -> Seq<(KeyView, Grp)> {
    match rank.at(okey(p.p1, p.p2, p.p3)) { Some(inn) => inn.entries(), None => Seq::empty() }
}

/// Every entry of the asked triple's rank index is a group of the answer, and in the answer's order.
pub proof fn lemma_ranked_full(db: Db, p: Params, groups: GMap, rank: GMap)
    requires
        gm_ok(groups), pos_ok(groups), rep_db(db), state_vals(groups, db), rank_ok(groups, rank), nm_ok(rank),
    ensures
        full_result(db, p, g_rows(p.p2, triple_entries(rank, p))),
        forall|i: int| 0 <= i < triple_entries(rank, p).len() ==> bits_ok(#[trigger] triple_entries(rank, p)[i].1),
{
    let ok = okey(p.p1, p.p2, p.p3);
    let entries = triple_entries(rank, p);
    let full = g_rows(p.p2, entries);
    if rank.at(ok) is Some {
        rank.at(ok)->0.model();
    }
    // each entry is a stored group with the asked key's parameters
    assert forall|i: int| 0 <= i < entries.len() implies ({
        let g = (#[trigger] entries[i]).1;
        &&& gm_at(rank, ok, entries[i].0) == Some(g)
        &&& entries[i].0 == rkey(g)
        &&& gm_at(groups, ok, g.ik()) == Some(g)
        &&& bits_ok(g)
    }) by {
        assert(gm_at(rank, ok, entries[i].0) == Some(entries[i].1));
    }
    assert forall|i: int| 0 <= i < entries.len() implies row_ok(db, p, #[trigger] full[i]) by {
        let g = entries[i].1;
        let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: ov(g.name), tag: g.tag@, plabel: ov(g.plabel) };
        assert(gk_at(groups, k) == Some(g));
        assert(gv_c(groups, k) == cntk(db, k) && gv_s(groups, k) == sumk(db, k));
        assert(g.cnt@ > 0);
        lemma_abs_le(db, k);
        if g.cnt@ != 1 { lemma_rounded_within(g.bits, g.sum@, absk(db, k), cntk(db, k) - 1); }
        assert(k_params(k) == p);
        lemma_row_ok(db, p, k, f64_val(g.bits));
        assert(full[i] == row_of_group(p, k, f64_val(g.bits), cntk(db, k)));
    }
    assert forall|i: int, j: int| 0 <= i < j < full.len() implies
        !same_group(#[trigger] full[i], #[trigger] full[j]) && !row_lt(full[j], full[i]) by {
        let (gi, gj) = (entries[i].1, entries[j].1);
        assert(key_lt(entries[i].0, entries[j].0));
        if same_group(full[i], full[j]) {
            assert(gi.ik() == gj.ik());
            assert(gi == gj);
            lemma_key_order(entries[i].0, entries[j].0);
            assert(false);
        }
        let (bi, bj) = (gi.bits, gj.bits);
        lemma_desc_key(bi, bj);
        lemma_desc_key(bj, bi);
        if row_lt(full[j], full[i]) {
            assert(f64_val(bi) == f64_val(bj));
            assert(f64_desc_key(bi) == f64_desc_key(bj));
            assert(bytes_lt(gi.ik().1, gj.ik().1));
            lemma_ikey_lt(ov(gj.name), gj.tag@, ov(gj.plabel), ov(gi.name), gi.tag@, ov(gi.plabel));
            lemma_bytes_lt_asym(gi.ik().1, gj.ik().1);
        }
    }
    assert forall|g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>|
        #[trigger] group_count(db, p, g_name, g_stmt, g_tag, g_plabel) > 0 && having(db, p, g_name, g_stmt, g_tag, g_plabel) implies
        exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).name == g_name && full[i].stmt == g_stmt && full[i].tag == g_tag && full[i].plabel == g_plabel by {
        lemma_group_nonempty(db, p, g_name, g_stmt, g_tag, g_plabel);
        let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag->0, plabel: g_plabel };
        assert(gv_c(groups, k) == cntk(db, k));
        assert(gk_at(groups, k) is Some);
        let g = gk_at(groups, k)->0;
        lemma_gm_ik(groups, ok, ikey(g_name, g_tag->0, g_plabel));
        lemma_ikey_inj(ov(g.name), g.tag@, ov(g.plabel), g_name, g_tag->0, g_plabel);
        assert(gm_at(rank, ok, rkey(g)) == Some(g));
        assert(rank.at(ok) is Some);
        let inn = rank.at(ok)->0;
        inn.model();
        let pos = choose|pos: int| #![auto] 0 <= pos < entries.len() && entries[pos].0 == rkey(g);
        assert(inn.at(entries[pos].0) == Some(entries[pos].1));
        assert(entries[pos].1 == g);
        assert(full[pos] == g_row(p.p2, g));
    }
}

/// The first 200 entries of the asked triple's rank index.
pub fn read_ranked(rank: &GMap, Ghost(groups): Ghost<GMap>, Ghost(db): Ghost<Db>, p1: i64, p2: &[u8], p3: &[u8]) -> (r: Option<Vec<OutT>>)
    requires
        gm_ok(groups), pos_ok(groups), rep_db(db), state_vals(groups, db), rank_ok(groups, *rank), nm_ok(*rank),
    ensures
        r matches Some(out) ==> query_result(db, params(p1 as int, p2@, p3@), out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, params(p1 as int, p2@, p3@)),
{
    let ghost p = params(p1 as int, p2@, p3@);
    let ok = okey_of(p1, p2, p3);
    let ghost entries = triple_entries(*rank, p);
    let ghost full = g_rows(p2@, entries);
    proof {
        lemma_ranked_full(db, p, groups, *rank);
        assert(ok.view() == okey(p.p1, p.p2, p.p3));
    }
    let mut out: Vec<OutT> = Vec::new();
    let inner = match rank.get(&ok) {
        None => {
            proof {
                assert(entries =~= Seq::<(KeyView, Grp)>::empty());
                assert(full =~= Seq::<OutRow>::empty());
                assert(out_view(out@) =~= limit_rows(full, 200));
            }
            return Some(out);
        }
        Some(i) => i,
    };
    let mut cur = inner.cursor();
    proof { assert(rank.at(ok.view()) == Some(*inner)); assert(cur.remaining() =~= entries.skip(0)); }
    let mut t: usize = 0;
    let mut more = true;
    while t < 200 && more
        invariant
            t <= 200, t <= entries.len(), out.len() == t,
            p == params(p1 as int, p2@, p3@),
            entries == triple_entries(*rank, p), full == g_rows(p2@, entries),
            full_result(db, p, full),
            forall|i: int| 0 <= i < entries.len() ==> bits_ok(#[trigger] entries[i].1),
            cur.remaining() == entries.skip(t as int),
            !more ==> t == entries.len(),
            out_view(out@) == full.take(t as int),
            forall|j: int| 0 <= j < t ==> out_fits(full[j]),
        decreases (200 - t) * 2 + if more { 1int } else { 0int },
    {
        let ghost before = out@;
        match cur.next() {
            None => {
                more = false;
                proof { assert(entries.skip(t as int).len() == 0); }
            }
            Some((key, g)) => {
                proof {
                    assert(entries.skip(t as int)[0] == entries[t as int]);
                    assert(entries.skip(t as int).skip(1) =~= entries.skip(t as int + 1));
                    assert(full[t as int] == g_row(p2@, *g));
                }
                let c = cnt_i64(&g.cnt);
                if c.is_none() {
                    proof {
                        assert(!out_fits(full[t as int]));
                        lemma_not_fits(db, p, full, t as int);
                    }
                    return None;
                }
                let tup: OutT = (copy_opt(&g.name), Some(copy_bytes(p2)), Some(copy_bytes(g.tag.as_slice())), copy_opt(&g.plabel), Some(g.bits), c.unwrap());
                out.push(tup);
                proof {
                    lemma_f64_in_range(g.bits);
                    assert(out_view(out@) =~= out_view(before).push(full[t as int]));
                    assert(full.take(t as int + 1) =~= full.take(t as int).push(full[t as int]));
                    assert(out_fits(full[t as int]));
                }
                t += 1;
            }
        }
    }
    proof {
        assert(full.len() == entries.len());
        if t < 200 { assert(!more); assert(full.take(t as int) =~= full); }
        assert(out_view(out@) =~= limit_rows(full, 200)) by {
            if full.len() <= 200 { assert(full.take(t as int) =~= full); }
        }
        assert forall|j: int| 0 <= j < out.len() implies out_fits(#[trigger] out_view(out@)[j]) by {
            assert(out_view(out@)[j] == full[j]);
        }
    }
    Some(out)
}

} // verus!
