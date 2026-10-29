// The host's rows as bag rows (view equality, copies) and, for sub, its Index key.
use vstd::prelude::*;
use verus_common::bag::*;
use verus_common::bytes::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::bucket::*;
use crate::f64bits::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::keys::*;

verus! {

pub fn copy_bytes(b: &[u8]) -> (out: Vec<u8>)
    ensures out@ == b@,
{
    vstd::slice::slice_to_vec(b)
}

pub fn same_int(a: i64, a_null: u8, b: i64, b_null: u8) -> (r: bool)
    ensures
        r == ((if a_null != 0 { None } else { Some(a as int) }) == (if b_null != 0 { None } else { Some(b as int) })),
{
    if a_null != 0 || b_null != 0 { a_null != 0 && b_null != 0 } else { a == b }
}

pub fn same_text(a: &Vec<u8>, a_null: u8, b: &Vec<u8>, b_null: u8) -> (r: bool)
    ensures
        r == ((if a_null != 0 { None } else { Some(a@) }) == (if b_null != 0 { None } else { Some(b@) })),
{
    if a_null != 0 || b_null != 0 { a_null != 0 && b_null != 0 } else { bytes_eq_exec(a.as_slice(), b.as_slice()) }
}

fn finite(bits: u64) -> (r: bool)
    ensures r == f64_finite(bits),
{
    proof { lemma_pow2_consts(); }
    (bits / 4503599627370496) % 2048 != 2047
}

fn zero(bits: u64) -> (r: bool)
    ensures r == (f64_mag_bits(bits) == 0),
{
    proof { lemma_pow2_consts(); }
    bits % 9223372036854775808 == 0
}

fn same_double(a: u64, a_null: u8, b: u64, b_null: u8) -> (r: bool)
    ensures
        r == ((if a_null != 0 { None } else { Some(f64_val(a)) }) == (if b_null != 0 { None } else { Some(f64_val(b)) })),
{
    if a_null != 0 || b_null != 0 {
        a_null != 0 && b_null != 0
    } else {
        proof {
            lemma_f64_in_range(a);
            lemma_f64_in_range(b);
            if f64_finite(a) && f64_finite(b) { lemma_f64_val_eq(a, b); }
        }
        if finite(a) && finite(b) { a == b || (zero(a) && zero(b)) } else { !finite(a) && !finite(b) }
    }
}

fn dup_text(v: &Vec<u8>) -> (r: Vec<u8>)
    ensures r@ == v@,
{
    copy_bytes(v.as_slice())
}

impl BagRow for SubRt {
    type V = SubRow;

    open spec fn rv(&self) -> SubRow {
        sub_rt_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        bytes_eq_exec(self.adsh.as_slice(), o.adsh.as_slice())
            && same_int(self.cik, self.cik__null, o.cik, o.cik__null)
            && same_text(&self.name, self.name__null, &o.name, o.name__null)
            && same_int(self.fy, self.fy__null, o.fy, o.fy__null)
    }

    fn dup(&self) -> (r: Self) {
        SubRt {
            adsh: dup_text(&self.adsh),
            cik: self.cik, cik__null: self.cik__null,
            name: dup_text(&self.name), name__null: self.name__null,
            fy: self.fy, fy__null: self.fy__null,
        }
    }
}

impl Keyed for SubRt {
    open spec fn vkey(v: SubRow) -> Option<KeyView> {
        Some(key1(v.adsh))
    }

    fn key(&self) -> (k: Option<Key>) {
        Some(make_key1(self.adsh.as_slice()))
    }
}

impl BagRow for NumRt {
    type V = NumRow;

    open spec fn rv(&self) -> NumRow {
        num_rt_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        same_text(&self.adsh, self.adsh__null, &o.adsh, o.adsh__null)
            && same_text(&self.uom, self.uom__null, &o.uom, o.uom__null)
            && same_double(self.value, self.value__null, o.value, o.value__null)
    }

    fn dup(&self) -> (r: Self) {
        NumRt {
            adsh: dup_text(&self.adsh), adsh__null: self.adsh__null,
            uom: dup_text(&self.uom), uom__null: self.uom__null,
            value: self.value, value__null: self.value__null,
        }
    }
}

}
