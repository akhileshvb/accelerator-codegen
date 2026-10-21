// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

use vstd::prelude::*;
pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod output;
pub mod facts;
pub mod join;
pub mod bag_proofs;
pub mod store;
pub mod batch;
pub mod core;
pub mod merge;
pub mod read;
pub mod changes;

