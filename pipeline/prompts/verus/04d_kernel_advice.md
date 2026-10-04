# Verus pipeline: a second opinion on a stuck kernel ({{benchmark}} q{{query}}, crate {{crate}})

Another agent is writing this kernel (phase: {{phase}}). Its last fix rounds
made no progress against the gate below. Look at the problem afresh and tell
it what is wrong and how to get past it. **Do not change any file** (changes
are undone); read the workspace and use the tools only to diagnose.

The failure:

```
{{failure}}
```

Work out the root cause, not the symptom:

* For failing tests: which write or parameter breaks it, what the kernel
  computes there and what it should (the spec in `crate/src/gen_spec.rs`, the
  declared inputs in `inputs/q.inputs`, the tests in `tests/`). `verus_sql`
  runs SQL on the test data; `verus_test(proofs=false)` reruns the tests.
* For failing proofs: which top-level fact (build / apply / execute) fails,
  and which missing invariant or lemma instantiation it needs
  (`contracts/proof_method.md` maps each obligation shape to its standard
  proof; `verus_verify` reruns Verus).
* Say so if the approach itself is the problem (a structure whose proof is
  too large, a design that cannot meet an obligation) and what to switch to.

Write `analysis/kernel_advice.md` (at most about 40 lines): the root cause,
the evidence (a test, a value, a Verus error line), and a concrete plan: what
to change in which function, in order. No preamble.

{{include:shared/rules.md}}
