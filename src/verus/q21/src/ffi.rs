// C ABI for the host (TRUSTED GLUE, not verified).
//
// These shims only turn raw pointers into slices and call the verified
// functions.  Ghost arguments are erased at compile time; the host's scans
// and change capture are what make the verified functions' preconditions
// true (see build::build_contract, orderspec::db_step, maint::scan_contract).
//
// Ownership: states and results are heap objects owned by the caller and
// released with q21v_free / q21v_result_free.  A state is immutable once
// returned; q21v_apply returns a new state and leaves its input intact.
use std::ptr;
use vstd::prelude::Ghost;

use crate::build::build;
use crate::execute::execute;
use crate::maint::{apply, needs_scan};
use crate::state::{LiDelta, LiRow, NatE, OrdDelta, State, SupE};

// Safely view a C pointer plus length as a Rust slice, mapping a null pointer or zero length to an empty slice.
unsafe fn slice<'a, T>(p: *const T, n: usize) -> &'a [T] {
    if n == 0 || p.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(p, n)
    }
}

// Decode a packed list of strings (one byte buffer plus n+1 boundary offsets) into one owned byte vector per row.
unsafe fn names<'a>(bytes: *const u8, off: *const usize, n: usize) -> Vec<Vec<u8>> {
    let off = slice(off, n + 1);
    let total = if n == 0 { 0 } else { off[n] };
    let bytes = slice(bytes, total);
    (0..n).map(|i| bytes[off[i]..off[i + 1]].to_vec()).collect()
}

/// Build from scans.  Names are concatenated in `*_names` with n+1 offsets.
/// Returns NULL on arithmetic overflow; the caller then uses SQL.
#[no_mangle]
pub unsafe extern "C" fn q21v_build(
    n_sup: usize,
    sup_sk: *const i64,
    sup_nk: *const i32,
    sup_names: *const u8,
    sup_off: *const usize,
    n_nat: usize,
    nat_nk: *const i32,
    nat_names: *const u8,
    nat_off: *const usize,
    n_f: usize,
    fkeys: *const i64,
    n_li: usize,
    lis: *const LiRow,
) -> *mut State {
    let sk = slice(sup_sk, n_sup);
    let snk = slice(sup_nk, n_sup);
    let sn = names(sup_names, sup_off, n_sup);
    let sups: Vec<SupE> = sn
        .into_iter()
        .enumerate()
        .map(|(i, name)| SupE { sk: sk[i], name, nk: snk[i] })
        .collect();
    let nnk = slice(nat_nk, n_nat);
    let nn = names(nat_names, nat_off, n_nat);
    let nats: Vec<NatE> = nn
        .into_iter()
        .enumerate()
        .map(|(i, name)| NatE { nk: nnk[i], name })
        .collect();
    match build(sups, nats, slice(fkeys, n_f), slice(lis, n_li), Ghost::assume_new()) {
        Some(s) => Box::into_raw(Box::new(s)),
        None => ptr::null_mut(),
    }
}

#[no_mangle]
// Free the maintained query state that the host got from build or apply; the host must not use it afterwards.
pub unsafe extern "C" fn q21v_free(s: *mut State) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

/// Writes the orderkeys the host must scan (on the post-transaction
/// database) into `out` (capacity n_od); returns how many.
#[no_mangle]
pub unsafe extern "C" fn q21v_needs_scan(
    s: *const State,
    n_od: usize,
    od: *const OrdDelta,
    out: *mut i64,
) -> usize {
    let keys = needs_scan(&*s, slice(od, n_od));
    let n = keys.len().min(n_od);
    for (i, k) in keys.iter().take(n).enumerate() {
        *out.add(i) = *k;
    }
    n
}

/// One transaction's maintenance.  Returns the new state, or NULL with
/// *err = 1 (unscanned new 'F' order) / 2 (overflow or inconsistent deltas);
/// the caller rebuilds.  Supplier / nation deltas: parallel arrays, names
/// concatenated with n+1 offsets, sign +1 / -1.
#[no_mangle]
pub unsafe extern "C" fn q21v_apply(
    s: *const State,
    n_od: usize,
    od: *const OrdDelta,
    n_ld: usize,
    ld: *const LiDelta,
    n_sd: usize,
    sd_sk: *const i64,
    sd_nk: *const i32,
    sd_names: *const u8,
    sd_off: *const usize,
    sd_sign: *const i64,
    n_nd: usize,
    nd_nk: *const i32,
    nd_names: *const u8,
    nd_off: *const usize,
    nd_sign: *const i64,
    n_sk: usize,
    skeys: *const i64,
    n_sr: usize,
    srows: *const LiRow,
    err: *mut i32,
) -> *mut State {
    let (ssk, snk, ssg) = (slice(sd_sk, n_sd), slice(sd_nk, n_sd), slice(sd_sign, n_sd));
    let sd: Vec<(SupE, i64)> = names(sd_names, sd_off, n_sd)
        .into_iter()
        .enumerate()
        .map(|(i, name)| (SupE { sk: ssk[i], name, nk: snk[i] }, ssg[i]))
        .collect();
    let (nnk, nsg) = (slice(nd_nk, n_nd), slice(nd_sign, n_nd));
    let nd: Vec<(NatE, i64)> = names(nd_names, nd_off, n_nd)
        .into_iter()
        .enumerate()
        .map(|(i, name)| (NatE { nk: nnk[i], name }, nsg[i]))
        .collect();
    match apply(
        &*s,
        slice(od, n_od),
        slice(ld, n_ld),
        &sd,
        &nd,
        slice(skeys, n_sk),
        slice(srows, n_sr),
        Ghost::assume_new(),
        Ghost::assume_new(),
    ) {
        Ok(s2) => {
            if !err.is_null() {
                *err = 0;
            }
            Box::into_raw(Box::new(s2))
        }
        Err(e) => {
            if !err.is_null() {
                *err = e as i32;
            }
            ptr::null_mut()
        }
    }
}

// Query answer handed to the host: owned (supplier name, numwait) rows, read back through the accessor functions below.
pub struct Q21Result {
    rows: Vec<(Vec<u8>, i64)>,
}

/// The Q21 answer for :nation = p (bytes).  NULL on overflow (use SQL).
#[no_mangle]
pub unsafe extern "C" fn q21v_execute(s: *const State, p: *const u8, plen: usize) -> *mut Q21Result {
    match execute(&*s, slice(p, plen)) {
        Some(rows) => Box::into_raw(Box::new(Q21Result { rows })),
        None => ptr::null_mut(),
    }
}

#[no_mangle]
// How many answer rows (suppliers) the result holds.
pub unsafe extern "C" fn q21v_result_len(r: *const Q21Result) -> usize {
    let rows = &(*r).rows;
    rows.len()
}

#[no_mangle]
// Supplier name of answer row i as raw bytes (valid until the result is freed); its length goes into *len.
pub unsafe extern "C" fn q21v_result_name(r: *const Q21Result, i: usize, len: *mut usize) -> *const u8 {
    let rows = &(*r).rows;
    let name = &rows[i].0;
    *len = name.len();
    name.as_ptr()
}

#[no_mangle]
// numwait of answer row i: how many orders that supplier kept waiting.
pub unsafe extern "C" fn q21v_result_count(r: *const Q21Result, i: usize) -> i64 {
    let rows = &(*r).rows;
    rows[i].1
}

#[no_mangle]
// Free an answer returned by execute; its name pointers become invalid.
pub unsafe extern "C" fn q21v_result_free(r: *mut Q21Result) {
    if !r.is_null() {
        drop(Box::from_raw(r));
    }
}
