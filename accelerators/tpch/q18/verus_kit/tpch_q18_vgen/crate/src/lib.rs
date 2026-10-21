// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

use vstd::prelude::*;
pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod pages;
pub mod data;
pub mod output;
pub mod model;
pub mod bounds;
pub mod maintenance;
pub mod gen_world;
pub mod gen_ffi;

verus! {
global size_of usize == 8;
}
