// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod compound_map;
pub mod contributions;
pub mod dimensions;
pub mod join_proofs;
pub mod base_updates;
pub mod base_proofs;
pub mod index_proofs;
pub mod routing;
pub mod route_proofs;
pub mod route_updates;

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod table;
pub mod dictionary;
pub mod model;
pub mod maintenance;
pub mod reading;
pub mod sql_bridge;
pub mod query_product;
pub mod query_core;
pub mod aggregate_bridge;
pub mod aggregate_cells;
pub mod aggregate_updates;
pub mod gen_world;
pub mod gen_ffi;
