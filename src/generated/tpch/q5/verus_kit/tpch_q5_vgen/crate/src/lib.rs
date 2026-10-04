// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod storage;
pub mod catalog;
pub mod meaning;
pub mod maintenance;
pub mod output;
pub mod construction;
pub mod query_proofs;
pub mod query_profiles;
pub mod query_catalog;
pub mod updates;
pub mod update_catalog;
pub mod profile_changes;
pub mod maintenance_proofs;
