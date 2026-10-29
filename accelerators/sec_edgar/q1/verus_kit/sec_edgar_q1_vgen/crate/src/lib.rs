// Verus kernel crate.  gen_* are generated and frozen; the kernel modules are written.
#![allow(unused_imports)]

use vstd::prelude::*;

verus! {
global size_of usize == 8;
}

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod model;
pub mod steps;
pub mod absorb;
pub mod bridge;
pub mod answer;
pub mod copy;
