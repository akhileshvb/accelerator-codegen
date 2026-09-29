# CorrespondsCppTwin / SOURCE MAP (Q1)

**Status:** see canonical [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md)
(§1 Twin↔Aux proved, §3 `cpp_model`). HB locus table:
[`TWIN_AUX_HARD_BRANCHES.md`](TWIN_AUX_HARD_BRANCHES.md).

## CorrespondsCppTwin claim

| Layer | Artifact | Role |
|-------|----------|------|
| C++ aux | `src/generated/tpch/q1/postgres/aux/module.cpp` | implementation |
| Extract harness | `scripts/run_dafny_q1_extract_diff.sh` (`finite_test`) | machine-checked finite samples |
| ModuleTwin | `q1/dafny/Q1ModuleTwin.dfy` | Dafny model of C++ |
| Twin↔Aux | `q1/dafny/Q1TwinAuxRefinement.dfy` + `Q1TwinAuxClose.dfy` | closed package; SPI = `AggsFaithful` / free rebuild oracle |

**CorrespondsCppTwin** = extract-diff + this SOURCE MAP pin, **not** a universal
C++↔Twin proof. Manifest kind: `cpp_model`.

Do not reintroduce `twin_aux_refinement` catch-all TCB.
