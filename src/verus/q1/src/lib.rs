// TPC-H Q1 stateless accelerator kernel, verified with Verus against a
// hand-written reference specification of the catalog SQL (spec.rs).
#![allow(unused_imports)]

pub mod spec;
pub mod kernel;
pub mod ffi;
