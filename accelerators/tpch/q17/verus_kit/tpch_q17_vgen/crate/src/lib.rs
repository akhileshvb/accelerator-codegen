// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
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
pub mod storage;
pub mod ordering;
pub mod model;
pub mod aggregation;
pub mod maintenance;
pub mod query_proofs;
pub mod group_updates;
pub mod changes;
pub mod semantics;
pub mod variants;
pub mod patches;
pub mod maintenance_semantics;
pub mod bounds;
