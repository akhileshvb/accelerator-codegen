# Verus pipeline: optimization attempt ({{benchmark}} q{{query}})

Make one measured improvement to the accelerator, keeping it verified and
correct. The pipeline accepts your change only if it still verifies with
nothing added to the trusted base, passes the frozen tests (differential and
concurrent snapshots, Verus store), and raises the score (read speedup ×
write-mix speedup, measured by the pipeline on SF1) by more than the noise.
Otherwise your change is reverted.

Current best score: {{best_score}} (run-to-run noise {{noise_pct}}%).

Attempts so far:

{{history}}

Profile of the current best (perf on an SF1 bench; `share` is the fraction
of samples):

```
{{profile}}
```

Exploration still owed before the loop may stop:

{{owed}}

Message from the pipeline: {{message}}

How to work:

* Pick a target from the profile (or the owed list): a kernel function, the
  read path, maintenance (writes), or a structural change of the aux state.
  Read `contracts/scale_principles.md`: prefer changes that also hold at
  SF3000.
* Measure with `verus_bench` (write mixes `{{mixes}}` this attempt) and
  `verus_profile`; check with `verus_verify` and `verus_test`
  (`contracts/proof_method.md` for keeping the proofs; prefer `vstd` collections).
* A structural change may edit `inputs/q.inputs` (the obligations are then
  regenerated and audited, the host glue is rewritten by the glue agent and
  re-checked, and the kernel must verify against the new obligations);
  {{structural_left}} such change(s) left. The glue is otherwise frozen.
* Record what you learned (what helped, what did not, numbers) with
  `playbook_append` (section `performance`, or `proofs` for proof-cost
  lessons).

End with a ```json block:
`{"target": "<hotspot name from the profile, or 'structural'>", "axis": "read" | "maintenance" | "both", "structural": true|false, "summary": "...", "infeasible": {"<hotspot>": "<why it cannot be improved>"}, "structural_considered": true|false}`.
If you believe nothing further is worth trying, write `OPTIMIZATION_SATURATED`
before the block and explain why; the pipeline accepts it only once the owed
exploration is done.

{{include:shared/rules.md}}
