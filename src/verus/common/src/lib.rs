// Query-independent Verus library for verified accelerator kernels:
// bag sums over tables, merge sort, C-collation byte order, exact rationals.
#![allow(unused_imports)]

pub mod sums;
pub mod sort;
pub mod bytes;
pub mod bag;
pub mod rat;
pub mod order;
pub mod rel;
pub mod sqlfns;
pub mod perm;
