// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod gen_world;
pub mod gen_ffi;
pub mod paged;
pub mod ents;
pub mod customers;
pub mod values;
pub mod facts;
pub mod dimensions;
pub mod model;
pub mod maintenance;
pub mod output;
pub mod calendar;
pub mod encoding;
pub mod key_facts;
pub mod query_proofs;
pub mod capture_proofs;
pub mod scan_proofs;
pub mod calendar_proofs;
pub mod maintenance_proofs;
pub mod dimension_updates;
pub mod query_semantics;
pub mod read_proofs;
pub mod dimension_query;
pub mod group_proofs;
pub mod selection;
pub mod completeness;
