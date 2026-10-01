// TPC-H Q1 stateless accelerator kernel, verified with Verus against the
// reference specification generated from the catalog SQL (gen_spec.rs, by
// sql2verus).
#![allow(unused_imports)]

pub mod gen_spec;
pub mod bridge;
pub mod kernel;
pub mod ffi;
