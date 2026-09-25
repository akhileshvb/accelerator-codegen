# FormalSQL / Logos integration

Pin: [PIN.md](PIN.md). Pipeline: [`../PIPELINE.md`](../PIPELINE.md).

## Layout

```text
formalsql/
  PIN.md
  README.md
  RUNTIME_GAPS.md
  lib/QuerySpineFacade.v          # generic TFGOP constructor facade (Q1)
  lib/QueryJoinSpineFacade.v      # CrossJoin ladder facade (Q21)
  lib/FilterAcceptanceFacade.v    # filter acceptance + temporal leb bridges
  lib/TemporalPredAcceptanceFacade.v  # DATE≤CAST+INTERVAL pred acceptance
  lib/NumericCallExactFacade.v    # NUMERIC Mul/Sub/Add/Cast + $f4/$f5 shapes
  lib/NumericFoldFacade.v         # Z-sum over rev/app (group bags)
  lib/NumericScaledFits.v         # of_scaled / digit-budget
  lib/ListNoDupFacade.v           # NoDup(map), filter uniqueness
  lib/KeyedSortFacade.v           # sorted + perm ⇒ equality
  lib/ProjectExactFacade.v        # project exact-map wrap
  lib/OrderRowsFacade.v           # canonical ORDER BY observation
  lib/GroupBagExactFacade.v      # group-bag success from exact groups
  lib/GroupsExactFacade.v         # groups list exact filter/map + bag combine
  lib/AggregateLeafExactFacade.v  # COUNT-star / SUM(dot) leaf exact
  resultspec/                     # ResultSpec Data/Eval (query-independent core)
  generated/                      # AuxSpec Rocq (auxspec/gen_auxspec.py)
  q1/                             # see q1/README.md + q1/STATUS.md
  q21/                            # see q21/STATUS.md
  scripts/                        # compile_*_proof.sh, check_*_rocq_no_admitted.sh
```

PGA recipe + Dafny SortedUniqueMap: [`../patterns/`](../patterns/).
Reuse inventory: [`../REUSE_LIBRARY_REPORT.md`](../REUSE_LIBRARY_REPORT.md).


## Lowering

Exact catalog `base_sql` → Logos (`scripts/logos_lower_catalog.sh q*`) →
`q*/generated/{Schema,Queries,Witness}.v`. Failure ⇒ not FormalSQL-verified.

## Q1 Box 1

Membership after Embed + fixed obligations + spine facade. Details: **`q1/STATUS.md`**.

```bash
scripts/compile_q1_proof.sh
scripts/check_q1_rocq_no_admitted.sh
```

## Q21 Box 1

Join-spine empty AuxCorrect + ContribEq. Details: **`q21/STATUS.md`**.

```bash
scripts/compile_q21_proof.sh
scripts/check_q21_rocq_no_admitted.sh
```
