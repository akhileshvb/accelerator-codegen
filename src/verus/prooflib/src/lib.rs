// The proof library: lemmas shared by Verus kernels, written by the pipeline's
// agents (seeded empty).  Rules: a lemma is general (no query names; stated
// over the spec vocabulary of verus_common or general structures), verifies
// on its own without assume, says when to use it in a doc comment, and
// records where it came from.  Adding one re-verifies every kernel that uses
// the library (pipeline tool prooflib_publish).
#![allow(unused_imports)]
use vstd::prelude::*;

verus! {

} // verus!
