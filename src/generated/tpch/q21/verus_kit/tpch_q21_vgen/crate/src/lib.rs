// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod index;
pub mod aggregates;
pub mod maintenance;
pub mod output;
pub mod query_proofs;
pub mod build_proofs;
pub mod maintenance_proofs;
pub mod output_proofs;
pub mod neighborhood_proofs;
pub mod summaries;
pub mod contribution_proofs;
pub mod aggregation_proofs;
pub mod gen_world;
pub mod gen_ffi;
