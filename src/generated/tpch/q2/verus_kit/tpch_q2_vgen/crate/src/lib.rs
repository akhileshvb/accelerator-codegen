// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod storage;
pub mod bags;
pub mod model;
pub mod maintenance;
pub mod output;
pub mod semantics;
pub mod selection;
pub mod enumeration;
pub mod query_transfer;
pub mod relational;
pub mod minimum_proofs;
pub mod result_proofs;
pub mod enumeration_proofs;
pub mod join_parts;
pub mod key_proofs;
pub mod query_proofs;
