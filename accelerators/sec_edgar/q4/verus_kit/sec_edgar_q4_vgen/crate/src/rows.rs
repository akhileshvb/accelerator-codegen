// The host's rows as bag rows (view equality, copies) and, for the join sides, their Index keys.
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
            && same_int(self.sic, self.sic__null, o.sic, o.sic__null)
    }

    fn dup(&self) -> (r: Self) {
        SubRt { adsh: dup_text(&self.adsh), cik: self.cik, cik__null: self.cik__null, sic: self.sic, sic__null: self.sic__null }
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

impl BagRow for TagRt {
    type V = TagRow;

    open spec fn rv(&self) -> TagRow {
        tag_rt_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        bytes_eq_exec(self.tag.as_slice(), o.tag.as_slice())
            && bytes_eq_exec(self.version.as_slice(), o.version.as_slice())
            && same_int(self.abstract_, self.abstract___null, o.abstract_, o.abstract___null)
            && same_text(&self.tlabel, self.tlabel__null, &o.tlabel, o.tlabel__null)
    }

    fn dup(&self) -> (r: Self) {
        TagRt {
            tag: dup_text(&self.tag), version: dup_text(&self.version),
            abstract_: self.abstract_, abstract___null: self.abstract___null,
            tlabel: dup_text(&self.tlabel), tlabel__null: self.tlabel__null,
        }
    }
}

impl Keyed for TagRt {
    open spec fn vkey(v: TagRow) -> Option<KeyView> {
        Some(key2(v.tag, v.version))
    }

    fn key(&self) -> (k: Option<Key>) {
        Some(make_key2(self.tag.as_slice(), self.version.as_slice()))
    }
}

impl BagRow for PreRt {
    type V = PreRow;

    open spec fn rv(&self) -> PreRow {
        pre_rt_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        same_text(&self.adsh, self.adsh__null, &o.adsh, o.adsh__null)
            && same_text(&self.stmt, self.stmt__null, &o.stmt, o.stmt__null)
            && same_text(&self.tag, self.tag__null, &o.tag, o.tag__null)
            && same_text(&self.version, self.version__null, &o.version, o.version__null)
    }

    fn dup(&self) -> (r: Self) {
        PreRt {
            adsh: dup_text(&self.adsh), adsh__null: self.adsh__null,
            stmt: dup_text(&self.stmt), stmt__null: self.stmt__null,
            tag: dup_text(&self.tag), tag__null: self.tag__null,
            version: dup_text(&self.version), version__null: self.version__null,
        }
    }
}

impl Keyed for PreRt {
    open spec fn vkey(v: PreRow) -> Option<KeyView> {
        if v.adsh is Some && v.tag is Some && v.version is Some { Some(key3(v.adsh->0, v.tag->0, v.version->0)) } else { None }
    }

    fn key(&self) -> (k: Option<Key>) {
        if self.adsh__null == 0 && self.tag__null == 0 && self.version__null == 0 {
            Some(make_key3(self.adsh.as_slice(), self.tag.as_slice(), self.version.as_slice()))
        } else {
            None
        }
    }
}

impl BagRow for NumRt {
    type V = NumRow;

    open spec fn rv(&self) -> NumRow {
        num_rt_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        same_text(&self.adsh, self.adsh__null, &o.adsh, o.adsh__null)
            && same_text(&self.tag, self.tag__null, &o.tag, o.tag__null)
            && same_text(&self.version, self.version__null, &o.version, o.version__null)
            && same_text(&self.uom, self.uom__null, &o.uom, o.uom__null)
            && same_double(self.value, self.value__null, o.value, o.value__null)
    }

    fn dup(&self) -> (r: Self) {
        NumRt {
            adsh: dup_text(&self.adsh), adsh__null: self.adsh__null,
            tag: dup_text(&self.tag), tag__null: self.tag__null,
            version: dup_text(&self.version), version__null: self.version__null,
            uom: dup_text(&self.uom), uom__null: self.uom__null,
            value: self.value, value__null: self.value__null,
        }
    }
}

}
