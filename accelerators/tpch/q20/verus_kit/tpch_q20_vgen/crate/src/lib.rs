// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod storage;
pub mod activation;
pub mod profiles;
pub mod dimensions;
pub mod model;
pub mod semantic_proofs;
pub mod output;
pub mod query_proofs;
pub mod profile_proofs;
pub mod state_proofs;
pub mod predicate_proofs;
pub mod read_proofs;
pub mod update_proofs;
