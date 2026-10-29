// Num rows as the kernel sees them: which group a row belongs to, and its signed contribution.
#![allow(unused_imports)]
#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::wide::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::wideval::*;

verus! {

/// Year bucket and day of year of a ddate, rounding down.
pub fn split_ddate(dd: i64) -> (r: (i64, i64))
    ensures r.0 as int == dd as int / 10000, r.1 as int == dd as int % 10000,
{
    let q = dd / 10000;
    let m = dd % 10000;
    if m < 0 { (q - 1, m + 10000) } else { (q, m) }
}

/// The key bytes of an optional text column.
pub fn opt_text(null: u8, b: &Vec<u8>) -> (r: Option<Vec<u8>>)
    ensures ov(r) == if null != 0 { None } else { Some(b@) },
{
    if null == 0 { Some(copy_bytes(b.as_slice())) } else { None }
}

/// Adds w times the row's contribution to the accumulator, if the row can be counted.
pub fn emit_num(tab: &Pow2Tab, acc: &mut Index<AccEnt>, r: &NumRt, w: i64)
    requires tab.ok(), acc_ok(*old(acc)),
    ensures
        acc_ok(*final(acc)),
        forall|k: GK| #![trigger acc_c(*final(acc), k)] acc_c(*final(acc), k) == acc_c(*old(acc), k) + w * ic(k)(num_rt_view(*r)),
        forall|k: GK| #![trigger acc_s(*final(acc), k)] acc_s(*final(acc), k) == acc_s(*old(acc), k) + w * iv(k)(num_rt_view(*r)),
{
    let ghost n = num_rt_view(*r);
    if r.value__null == 0 && r.uom__null == 0 && r.ddate__null == 0 {
        let (y, m) = split_ddate(r.ddate);
        if 101 <= m && m <= 1231 {
            let tag = opt_text(r.tag__null, &r.tag);
            let ver = opt_text(r.version__null, &r.version);
            let wv = wide_of_bits(tab, r.value);
            let dc = Wide::from_i128(w as i128);
            let ds = dc.mul(&wv);
            let ghost k0 = GK { y: y as int, uom: r.uom@, tag: ov(tag), ver: ov(ver) };
            let ghost old_acc = *acc;
            acc_add(acc, y, r.uom.as_slice(), &tag, &ver, &dc, &ds);
            proof {
                assert(dq(n));
                assert(gk_of(n) == k0);
                assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(old_acc, k) + w * ic(k)(n) by {
                    if k == k0 { assert(ic(k)(n) == 1); assert(w * 1 == w); } else { assert(ic(k)(n) == 0); assert(w * 0 == 0); }
                }
                assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(old_acc, k) + w * iv(k)(n) by {
                    if k == k0 { assert(iv(k)(n) == f64_val(r.value)); } else { assert(iv(k)(n) == 0); assert(w * 0 == 0); }
                }
            }
            return;
        }
    }
    proof {
        assert(!dq(n));
        assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(*acc, k) + w * ic(k)(n) by { assert(ic(k)(n) == 0); assert(w * 0 == 0); }
        assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(*acc, k) + w * iv(k)(n) by { assert(iv(k)(n) == 0); assert(w * 0 == 0); }
    }
}

} // verus!
