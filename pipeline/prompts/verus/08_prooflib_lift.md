# Verus pipeline: lift a proven structure into the shared proof library

Several kernels in this tree need the same verified data structure. Instead of each proving its own copy,
lift one that is already proved into the shared proof library (`prooflib/src/lib.rs` in this workspace,
published to every kernel as `verus_prooflib`), so they can all use it.

What to lift, and what the kernels need from it:

{{request}}

Rules:
* Generic and query-independent: no query, table or column names; generic value types; the key type and
  its order as general as the source allows (composite keys of integers and byte strings are fine).
* Self-contained: put it in its own module inside `lib.rs` (`pub mod <name> { ... }`), so its names cannot
  clash with names in existing kernels.
* Proved without `assume` / `admit` / `external_body`; keep the source's proofs and adapt them.
* Specification: tie the executable structure to a mathematical view (a `Map` / ordered sequence of
  entries), state each operation's effect on that view, and include the lemmas a kernel needs to keep a
  sum-over-entries invariant across an update.
* Persistent: an update returns a new version and leaves the old one valid, copying only the path it
  changes (`Arc`-shared nodes).
* Document in doc comments when to use it and its cost of each operation.

Verify your copy of the library as you go with the `prooflib_verify` tool (it checks the library on its
own, without publishing).

Publish with `prooflib_publish(note)` when it verifies (it re-verifies the library and every kernel that
already verified, and only accepts additions). Then end with a short summary: the module name, its
operations and their signatures, and how a kernel imports it.

{{include:shared/rules.md}}
