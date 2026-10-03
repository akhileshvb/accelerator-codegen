// Verus kernel crate (pipeline).  gen_* are generated and frozen; the
// kernel (kernel.rs and any modules it adds) is written by the agents.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod pages;
pub mod data;
pub mod output;
pub mod model;
pub mod maintenance;
pub mod gen_world;
pub mod gen_ffi;
