# The trusted base

Trusted (not proved): the spec generated from the SQL (frozen, audited), the
scan / capture contracts in gen_obligations.rs (the host's promises), the
generated glue and C ABI, vstd and the Verus toolchain.  Query parameters are
non-NULL unless the spec declares them `Option` (`None` = NULL, three-valued like a
NULL-able column); the host answers with SQL for a NULL it cannot pass.

Proved: everything in your kernel. So your files may NOT contain (the gate
rejects them):

* `assume(...)`, `admit(...)`, `Ghost::assume_new` / `Tracked::assume_new`
* `#[verifier::external_body]`, `#[verifier::external]`,
  `assume_specification` / `external_fn_specification`
* `unsafe`, `extern "C"`, `#[no_mangle]` (the C ABI is generated)
* I/O or process access (`std::fs`, `std::net`, `std::process`, `std::env`)

and must not edit frozen files (spec, gen_*, frozen/, tests once frozen).
Ordinary vstd library calls (Vec, HashMap, Arc, ...) are fine: their specs
are part of vstd.
