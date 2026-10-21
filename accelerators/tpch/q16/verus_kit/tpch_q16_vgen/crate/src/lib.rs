// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod vocab;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod model;
pub mod counts;
pub mod output;
pub mod maintenance;
pub mod encoding;
pub mod semantic_proofs;
pub mod input_proofs;
pub mod output_proofs;
pub mod maintenance_proofs;
pub mod rank_proofs;
pub mod build_semantics;
pub mod query_semantics;
pub mod output_validation;
pub mod maintenance_semantics;
pub mod bounds;
pub mod contrib;
