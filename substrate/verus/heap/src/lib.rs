//! Measured heap of a Verus kernel library (TRUSTED, not verified): a counting global allocator.
//!
//! Every crate graph that contains this crate allocates through it, so a kit's (or a world's)
//! static library reports the bytes its Rust code holds live, which is the verified store's
//! state (all versions kept) plus transients in flight.  The C ABI of each kit exposes
//! `heap_bytes` / `heap_peak_bytes` / `heap_reset_peak` over these functions.
//!
//! Counting is sharded per thread (a thread adds to its own cache line) so parallel builds do
//! not contend; the peak is sampled whenever a shard has grown by `PEAK_STEP` since its last
//! sample, so it is exact to within `SHARDS * PEAK_STEP`.
// Verus compiles every crate it is given and wants vstd in it, though nothing here is verified.
#[allow(unused_imports)]
use vstd::prelude::*;

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::atomic::{AtomicI64, AtomicUsize, Ordering::Relaxed};

const SHARDS: usize = 64;
const PEAK_STEP: i64 = 1 << 20;

#[repr(align(64))]
struct Shard {
    live: AtomicI64,
    grown: AtomicI64,
}

#[allow(clippy::declare_interior_mutable_const)]
const EMPTY: Shard = Shard { live: AtomicI64::new(0), grown: AtomicI64::new(0) };
static SHARD: [Shard; SHARDS] = [EMPTY; SHARDS];
static NEXT_SHARD: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);

thread_local! {
    static MINE: Cell<usize> = const { Cell::new(usize::MAX) };
}

fn shard() -> &'static Shard {
    let i = MINE.with(|m| {
        if m.get() == usize::MAX {
            m.set(NEXT_SHARD.fetch_add(1, Relaxed) % SHARDS);
        }
        m.get()
    });
    &SHARD[i]
}

fn grow(n: usize) {
    let s = shard();
    s.live.fetch_add(n as i64, Relaxed);
    if s.grown.fetch_add(n as i64, Relaxed) + n as i64 >= PEAK_STEP {
        s.grown.store(0, Relaxed);
        PEAK.fetch_max(live_bytes(), Relaxed);
    }
}

fn shrink(n: usize) {
    shard().live.fetch_sub(n as i64, Relaxed);
}

/// Bytes allocated and not yet freed.
pub fn live_bytes() -> usize {
    SHARD.iter().map(|s| s.live.load(Relaxed)).sum::<i64>().max(0) as usize
}

/// The most `live_bytes` reached since the start or the last `reset_peak`.
pub fn peak_bytes() -> usize {
    PEAK.load(Relaxed).max(live_bytes())
}

/// Start a new peak measurement from the current live bytes.
pub fn reset_peak() {
    PEAK.store(live_bytes(), Relaxed);
}

struct Counting;

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = System.alloc(l);
        if !p.is_null() {
            grow(l.size());
        }
        p
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        let p = System.alloc_zeroed(l);
        if !p.is_null() {
            grow(l.size());
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        System.dealloc(p, l);
        shrink(l.size());
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        let q = System.realloc(p, l, new);
        if !q.is_null() {
            if new >= l.size() {
                grow(new - l.size());
            } else {
                shrink(l.size() - new);
            }
        }
        q
    }
}

#[global_allocator]
static ALLOCATOR: Counting = Counting;
