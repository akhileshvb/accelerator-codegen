// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod maps;
pub mod profiles;
pub mod segments;
pub mod joins;
pub mod output;
pub mod proof_support;
pub mod query_proofs;
pub mod database_proofs;
