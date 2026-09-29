# Q1 current theorem / TCB summary

**Canonical status for Q1 verification.** Q1 follows the same recipe as Q21
(`../PIPELINE.md`). Other docs link here.

Gates: `scripts/run_verification_q1.sh`; the Q1 regressions inside
`q21/run_q21_verification.sh`. Residual manifest: [`RESIDUALS.txt`](RESIDUALS.txt).

## 1. Stage 2 (Rocq): FormalSQL = Embed(Eval_Spec rows)

`formalsql/q1/Q1FinalTheorem.v`:

```text
NormalizeSound_Q1_rel_all_outcomes :
  Preconditions_Q1 db δ -> Q1NumericSafe db ->
  Eval_Spec LogicalSpec_Q1_rel (param_cutoff (cutoff_Q1 δ)) (db_of_aux db) = Some rows ->
  exists outs, Embed_Q1_Result rows = Some outs /\
               FormalSQL_exec (EmbedDB_Q1 db) δ outs /\
               every FormalSQL outcome of q̂_Q1 δ = SqlSuccess outs
```

`Embed_Q1_Result` embeds the RelNode rows directly: keys as characters, sums
as scaled decimals (2 / 2 / 4 / 6), each AVG from its own `VAvgExact(sum,count)`
as PostgreSQL `avg(numeric)` (`NumericAggExact.avg_value`), count as int8.
Only the five standard classical axioms (Print Assumptions gate).

**Numeric embedding: proved, no longer a premise.**
`Q1NumericEmbed.NumericEmbedFaithful_holds`, from the query-generic
`formalsql/lib/NumericAggExact.v` (exact SUM over scaled decimals, exact
fixed-typmod AVG, `pg_avg_exact`: PostgreSQL's avg scale/rounding as an integer
closed form). Axiom-free.

*Correction recorded:* the former premise quantified over every row list,
including the empty one, and is false (`NumericEmbedFaithful_unrestricted_false`:
SUM over no rows is NULL). All Q1 theorems that assumed it were vacuous; the
corrected statement quantifies over non-empty, well-formed groups.

Internal steps (fully proved, not in any statement): the FormalSQL group
correspondence (`Q1GroupsCorrespond`) and the RelNode ↔ flat bridge
(`Q1RelNodeBridge`, `Q1EvalMigration`) meet at a Rocq-private group model
(`generated/Q1AuxSpec.v`, no longer dual-emitted); `Eval_Spec_Q1_rel_shape`
gives the row shape used by the embedding.

## 2. Stages 3-4 (Dafny): accelerator = Obs(Eval_Spec rows), exactly

`q1/dafny/Q1ExecExact.dfy`:

```text
Q1ExecExact : Inv(store, db) ==> ExecSeqM(db, cutoff) == Q1Rows(cutoff, db)
Q1Rows(c, db) = ObsRow* (Eval_Spec(LogicalSpec_Q1_rel, cutoff c, db))
```

`ObsRow` copies keys / sums and computes each AVG as
`PgNumeric.AvgOfScaledSum(VAvgExact.sum, 2, VAvgExact.count)` — exact; the
former float observation TCB (`AvgObservationFaithful`) is gone. Twin ↔ Aux:
`Q1TwinAuxRefinement`, `Q1TwinAuxClose` (closed execution + maintenance);
row shape: `Q1RelNodeBridge.Eval_Spec_Q1_rel_rows_shape`.

Remaining intentional twin contract: `AggsFaithful` for the aggregate-scan
BuildState arms (the lineitem BuildState arm, used by the runtime, does not
need it).

## 3. Stage 5 (Dafny): runtime

`q1/dafny/Q1Runtime.dfy` instantiates `substrate/dafny/AuxSyncProtocol.dfy`
(same protocol proof as Q21): `Build_step_Q1`, `Commit_step_Q1` (arbitrary
signed deltas; fold is multiset-invariant), `Hit_correct_Q1`,
`Fallback_correct_Q1`: Execute == `Q1Rows` of the database the statement sees.

## 4. Accelerator (C++)

Postgres Q1 emits exact PostgreSQL numeric text: sums from the 128-bit
accumulators, averages via `stateful/core/pg_numeric.hpp` (twin of
`PgNumeric.dfy`). Checked: Dafny twin == C++ on grid D (all lines exact);
helper == PostgreSQL on 20,000 random cases; live Q1 output text == PostgreSQL
on SF1.

## 5. Trusted edges

See [`RESIDUALS.txt`](RESIDUALS.txt).
