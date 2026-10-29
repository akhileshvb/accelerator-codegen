// A commit (or a build, which is a commit of every sub row onto the nums alone) applied to the state:
// stage A accumulates the changed rows' contributions, stage B merges them, the sub buckets take the changed rows.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::bucket::*;
use crate::dmap::*;
use crate::ent::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::keys::*;
use crate::merge::*;
use crate::numeric::*;
use crate::qty::*;
use crate::rows::*;
use crate::stagea::*;
use crate::wbag::*;

verus! {

/// The sub table the buckets stand for after the changes `ch`.
pub open spec fn phi_acc(tab: Seq<SubRow>, ch: Seq<(SubRow, int)>) -> Phi {
    |g: spec_fn(SubRow) -> int| sum(tab, g) + sum(ch, |e: (SubRow, int)| e.1 * g(e.0))
}

proof fn lemma_wbag_ext(b: Index<Vec<WRow>>, phi1: Phi, phi2: Phi)
    requires
        wbag_as(b, phi1),
        forall|g: spec_fn(SubRow) -> int| #[trigger] phi1(g) == phi2(g),
    ensures wbag_as(b, phi2),
{
    assert forall|k: KeyView, f: spec_fn(SubRow) -> int| #[trigger] wsum(wrows_at(b, k), f) == phi2(skw(k, f)) by {
        assert(phi1(skw(k, f)) == phi2(skw(k, f)));
    }
}

/// The sub buckets with the batch's changed sub rows.
pub fn sub_bag_apply(sb: &mut Index<Vec<WRow>>, sd: &Vec<SubDelta>, Ghost(tab): Ghost<Seq<SubRow>>)
    requires
        wbag_as(*old(sb), phi_tab(tab)),
        forall|i: int| 0 <= i < sd.len() ==> (#[trigger] sd[i]).sign == 1 || sd[i].sign == -1,
    ensures
        wbag_as(*final(sb), phi_acc(tab, sub_delta_seq_view(sd@))),
{
    let ghost view = sub_delta_seq_view(sd@);
    let mut i: usize = 0;
    proof {
        assert forall|g: spec_fn(SubRow) -> int| #[trigger] phi_tab(tab)(g) == phi_acc(tab, view.take(0))(g) by {
            assert(view.take(0) =~= Seq::<(SubRow, int)>::empty());
        }
        lemma_wbag_ext(*sb, phi_tab(tab), phi_acc(tab, view.take(0)));
    }
    while i < sd.len()
        invariant
            i <= sd.len(),
            view == sub_delta_seq_view(sd@),
            wbag_as(*sb, phi_acc(tab, view.take(i as int))),
            forall|j: int| 0 <= j < sd.len() ==> (#[trigger] sd[j]).sign == 1 || sd[j].sign == -1,
        decreases sd.len() - i,
    {
        wbag_add(sb, &sd[i].row, sd[i].sign, Ghost(phi_acc(tab, view.take(i as int))));
        proof {
            assert(view[i as int] == (sub_rt_view(sd[i as int].row), sd[i as int].sign as int));
            assert(view.take(i as int + 1) =~= view.take(i as int).push(view[i as int]));
            assert forall|g: spec_fn(SubRow) -> int| #[trigger] phi_add(phi_acc(tab, view.take(i as int)), sub_rt_view(sd[i as int].row), sd[i as int].sign as int)(g)
                == phi_acc(tab, view.take(i as int + 1))(g) by {
                lemma_sum_push(view.take(i as int), view[i as int], |e: (SubRow, int)| e.1 * g(e.0));
            }
            lemma_wbag_ext(*sb, phi_add(phi_acc(tab, view.take(i as int)), sub_rt_view(sd[i as int].row), sd[i as int].sign as int), phi_acc(tab, view.take(i as int + 1)));
        }
        i += 1;
    }
    proof { assert(view.take(sd.len() as int) =~= view); }
}

proof fn lemma_empty_ddm(dm: Index<DCls>)
    requires dm.wf(), forall|k: KeyView| #[trigger] dm.at(k) is None,
    ensures ddm_ok(dm, Seq::empty()),
{
}

proof fn lemma_key1_inj(a: Seq<u8>, b: Seq<u8>)
    requires key1(a) == key1(b),
    ensures a == b,
{
    assert(escape(a) + Seq::<u8>::empty() =~= escape(a));
    assert(escape(b) + Seq::<u8>::empty() =~= escape(b));
    lemma_escape_split(a, Seq::empty(), b, Seq::empty());
}

/// The buckets of the rescanned num rows hold, for the filings of the changed sub rows, the sums of the table after the commit.
pub proof fn lemma_nb_covers(nb: Index<Vec<NumRt>>, rows: Seq<NumRt>, tab: Seq<NumRow>, sd: Seq<SubDelta>, keys: Seq<Seq<u8>>)
    requires
        bag_holds::<NumRt>(nb, rows_v(rows)),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_rt_seq_view(rows), f)
            == sum(tab, |num: NumRow| if num.adsh is Some && keys.contains(num.adsh->0) { f(num) } else { 0 }),
        forall|i: int| 0 <= i < sd.len() ==> keys.contains((#[trigger] sd[i]).row.adsh@),
    ensures nb_covers(nb, tab, sd),
{
    assert(rows_v(rows) =~= num_rt_seq_view(rows));
    assert forall|i: int, f: spec_fn(NumRow) -> int| 0 <= i < sd.len() implies
        #[trigger] sum(rows_at::<NumRt>(nb, key1(sd[i].row.adsh@)), f) == sum(tab, key_weight::<NumRt>(key1(sd[i].row.adsh@), f)) by {
        let k = key1(sd[i].row.adsh@);
        let kw = key_weight::<NumRt>(k, f);
        assert(sum(rows_at::<NumRt>(nb, k), f) == sum(rows_v(rows), kw));
        assert(sum(num_rt_seq_view(rows), kw) == sum(tab, |num: NumRow| if num.adsh is Some && keys.contains(num.adsh->0) { kw(num) } else { 0 }));
        let cf = |n2: NumRow| if n2.adsh is Some && keys.contains(n2.adsh->0) { kw(n2) } else { 0 };
        assert forall|num: NumRow| #[trigger] cf(num) == kw(num) by {
            if kw(num) != 0 {
                assert(<NumRt as Keyed>::vkey(num) == Some(k));
                assert(num.adsh is Some);
                lemma_key1_inj(num.adsh->0, sd[i].row.adsh@);
                assert(keys.contains(num.adsh->0));
            }
        }
        lemma_sum_ext_all(tab, cf, kw);
    }
}

/// The state after a commit: the classes and the sub buckets.
pub fn apply_core(classes: &Index<Class>, subs: &Index<Vec<WRow>>, c: &Consts, nd: &Vec<NumDelta>, sd: &Vec<SubDelta>, nb: &Index<Vec<NumRt>>,
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (r: (Index<Class>, Index<Vec<WRow>>))
    requires
        consts_ok(*c),
        agg_ok(*classes, db),
        wbag_as(*subs, phi_tab(db.sub)),
        nums_dbl(db),
        nums_dbl(db2),
        bag_step(db.num, db2.num, num_delta_seq_view(nd@)),
        bag_step(db.sub, db2.sub, sub_delta_seq_view(sd@)),
        forall|i: int| 0 <= i < nd.len() ==> (#[trigger] nd[i]).sign == 1 || nd[i].sign == -1,
        forall|i: int| 0 <= i < sd.len() ==> (#[trigger] sd[i]).sign == 1 || sd[i].sign == -1,
        nb_covers(*nb, db2.num, sd@),
    ensures
        agg_ok(r.0, db2),
        wbag_as(r.1, phi_tab(db2.sub)),
{
    let mut dm: Index<DCls> = Index::new();
    proof { lemma_empty_ddm(dm); }
    let Ghost(l1) = stage_n(&mut dm, nd, subs, &c.pows, Ghost(db.sub));
    let Ghost(l) = stage_s(&mut dm, sd, nb, &c.pows, Ghost(l1), Ghost(db2.num));
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(l, p, h)
            == left_join_delta(num_delta_seq_view(nd@), db.sub, wt(p, h)) + right_join_delta(db2.num, sub_delta_seq_view(sd@), wt(p, h)) by {
            assert(lsum(l1, p, h) == left_join_delta(num_delta_seq_view(nd@), db.sub, wt(p, h)));
        }
        lemma_ledger_from_deltas(l, db, db2, num_delta_seq_view(nd@), sub_delta_seq_view(sd@));
    }
    let mut cs = classes.share();
    proof {
        assert(agg_ok(cs, db)) by {
            assert forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) == classes.at(ckey(fy as int, uom)) by {}
        }
    }
    merge_all(&mut cs, &dm, c, Ghost(db), Ghost(db2), Ghost(l));
    let mut sb = subs.share();
    proof {
        assert forall|k: KeyView, f: spec_fn(SubRow) -> int| #[trigger] wsum(wrows_at(sb, k), f) == phi_tab(db.sub)(skw(k, f)) by {
            assert(wrows_at(sb, k) == wrows_at(*subs, k));
        }
    }
    sub_bag_apply(&mut sb, sd, Ghost(db.sub));
    proof {
        let ch = sub_delta_seq_view(sd@);
        assert forall|g: spec_fn(SubRow) -> int| #[trigger] phi_acc(db.sub, ch)(g) == phi_tab(db2.sub)(g) by {
            assert(sum(db2.sub, g) == sum(db.sub, g) + sum(ch, |e: (SubRow, int)| e.1 * g(e.0)));
        }
        lemma_wbag_ext(sb, phi_acc(db.sub, ch), phi_tab(db2.sub));
    }
    (cs, sb)
}

}
