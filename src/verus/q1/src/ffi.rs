// C ABI for the host (TRUSTED GLUE, not verified).
//
// These shims only turn raw pointers into slices and call the verified
// functions.  The host's scans are what make `consume`'s batches the rows of
// lineitem (see the Q1 Verus README).  A state whose `consume` or `merge`
// refused (out-of-domain row) is poisoned: every later call refuses too, and
// the host answers with SQL.
#![allow(improper_ctypes_definitions)]
use crate::kernel::{OutGroup, Q1State};

pub struct Q1Handle {
    st: Q1State,
    ok: bool,
}

pub struct Q1Result {
    groups: Vec<OutGroup>,
}

unsafe fn slice<'a, T>(p: *const T, n: usize) -> &'a [T] {
    if n == 0 || p.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(p, n)
    }
}

#[no_mangle]
pub extern "C" fn q1v_new(cutoff: i32) -> *mut Q1Handle {
    Box::into_raw(Box::new(Q1Handle { st: Q1State::new(cutoff), ok: true }))
}

#[no_mangle]
pub unsafe extern "C" fn q1v_free(h: *mut Q1Handle) {
    if !h.is_null() {
        drop(Box::from_raw(h));
    }
}

/// Consume n rows given as parallel column arrays (blocked kernel: narrow
/// domain, i64 block-local sums).  Returns 1 on success, 0 if the state
/// refused (now or earlier).
#[no_mangle]
pub unsafe extern "C" fn q1v_consume(
    h: *mut Q1Handle,
    n: usize,
    shipdate: *const i32,
    quantity: *const i64,
    extendedprice: *const i64,
    discount: *const i64,
    tax: *const i64,
    returnflag: *const u8,
    linestatus: *const u8,
) -> u8 {
    let h = &mut *h;
    if !h.ok {
        return 0;
    }
    h.ok = h.st.consume(
        slice(shipdate, n),
        slice(quantity, n),
        slice(extendedprice, n),
        slice(discount, n),
        slice(tax, n),
        slice(returnflag, n),
        slice(linestatus, n),
    );
    h.ok as u8
}

/// Same contract, simple kernel: wide domain, i128 sums updated per row.
#[no_mangle]
pub unsafe extern "C" fn q1v_consume_simple(
    h: *mut Q1Handle,
    n: usize,
    shipdate: *const i32,
    quantity: *const i64,
    extendedprice: *const i64,
    discount: *const i64,
    tax: *const i64,
    returnflag: *const u8,
    linestatus: *const u8,
) -> u8 {
    let h = &mut *h;
    if !h.ok {
        return 0;
    }
    h.ok = h.st.consume_simple(
        slice(shipdate, n),
        slice(quantity, n),
        slice(extendedprice, n),
        slice(discount, n),
        slice(tax, n),
        slice(returnflag, n),
        slice(linestatus, n),
    );
    h.ok as u8
}

/// dst := dst ++ src (src is left intact).  Returns 1 on success.
#[no_mangle]
pub unsafe extern "C" fn q1v_merge(dst: *mut Q1Handle, src: *const Q1Handle) -> u8 {
    let dst = &mut *dst;
    let src = &*src;
    if !dst.ok || !src.ok || dst.st.cutoff != src.st.cutoff {
        dst.ok = false;
        return 0;
    }
    dst.ok = dst.st.merge(&src.st);
    dst.ok as u8
}

/// The answer, or NULL if the state refused.
#[no_mangle]
pub unsafe extern "C" fn q1v_finish(h: *const Q1Handle) -> *mut Q1Result {
    let h = &*h;
    if !h.ok {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new(Q1Result { groups: h.st.finish() }))
}

#[no_mangle]
pub unsafe extern "C" fn q1v_result_len(r: *const Q1Result) -> usize {
    (&(*r).groups).len()
}

#[no_mangle]
pub unsafe extern "C" fn q1v_result_get(r: *const Q1Result, i: usize) -> *const OutGroup {
    &(&(*r).groups)[i]
}

#[no_mangle]
pub unsafe extern "C" fn q1v_result_free(r: *mut Q1Result) {
    if !r.is_null() {
        drop(Box::from_raw(r));
    }
}
