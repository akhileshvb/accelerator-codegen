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
pub mod f64bits;
pub mod model;
pub mod bridge;
pub mod cells;
pub mod maint;
pub mod keys;
pub mod group;
pub mod filing;
pub mod agree;
pub mod rescan;
pub mod delta;
pub mod classes;
pub mod finish;
pub mod walk;
pub mod sortout;
