// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod values;
pub mod inventory;
pub mod ranks;
pub mod model;
pub mod maintenance;
pub mod semantics;
pub mod construction;
pub mod names;
pub mod counts;
pub mod staging;
pub mod join_proofs;
pub mod name_updates;
pub mod gen_world;
pub mod gen_ffi;
