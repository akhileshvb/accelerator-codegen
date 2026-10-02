// TPC-H Q21 accelerator kernel, verified with Verus against the reference
// specification generated from the catalog SQL (gen_spec.rs, by sql2verus).
// gen_* files are generated (scripts/gen_verus_specs.sh); the kernel is the rest.
#![allow(unused_imports)]

pub mod gen_spec;
pub mod gen_obligations;
pub mod kernel;
pub mod bridge;
pub mod orderspec;
pub mod ents;
pub mod state;
pub mod build;
pub mod execute;
pub mod maint;
// On the verified store (verus_substrate), and the C ABI of both stores.
pub mod gen_world;
pub mod gen_ffi;
