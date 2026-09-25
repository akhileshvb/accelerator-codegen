(* Q1NormalizeSound — Eval_FS(qhat) ≈ EvalSpec(S_Q1) for the Q1 TFGOP fragment.

   One-time amortized theorem: reuses FormalSQLAuxCorrect_Q1 + QuerySpineFacade;
   does NOT reopen AggregateLeafExact / GroupsExact leaf campaigns.

   Scope (honest):
   - Proved for [qhat_Q1 delta] (supported family normalize accepts for Q1).
   - With [normalize_qhat_Q1]: normalize(qhat)=Some(S) ∧ Eval_FS≈EvalSpec(S).
   - Generic ∀q. normalize(q)=Some(s)→… is NOT claimed: normalize peels
     TFGOP+lineitem+delta only (does not fingerprint selects/aggs).

   Residuals inherited from AuxCorrect:
   - NumericEmbedFaithful [TCB]
   - [Q1_GroupBagsPerm] discharged via SortedGroupResultFacade
*)

From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import QueryResultSpec NormalizeQuery EvalSpec.
From JetStreamFS Require Import Q1AuxSpec.

Require Import Q1FormalSQLProof.
Require Import Q1NumericEmbed.
Require Import Q1GroupsCorrespond.
Require Import Q1TCBAuxCorrect.
Require Import Q1ResultSpecNormalize.

(** * NormalizeSound_Q1 *)

Theorem NormalizeSound_Q1 :
  forall (db : list Q1AuxSpec.Row) (delta : Z),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (EvalSpec_Q1 (S_Q1 (cutoff_Q1 delta)) db)).
Proof.
  intros db delta Hpre Hsafe Hnum.
  rewrite EvalSpec_S_Q1.
  rewrite cutoff_Q1_eq_EmbedParams.
  exact (FormalSQLAuxCorrect_Q1 db delta Hpre Hsafe Hnum).
Qed.

Theorem Normalize_and_Sound_Q1 :
  forall (db : list Q1AuxSpec.Row) (delta : Z),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    normalize_query (qhat_Q1 delta) = Some (S_Q1 (cutoff_Q1 delta)) /\
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (EvalSpec_Q1 (S_Q1 (cutoff_Q1 delta)) db)).
Proof.
  intros db delta Hpre Hsafe Hnum.
  split.
  - apply normalize_qhat_Q1.
  - apply NormalizeSound_Q1; assumption.
Qed.

Theorem NormalizeSound_Q1_nil :
  forall delta,
    Preconditions_Q1 nil delta ->
    FormalSQL_exec (EmbedDB_Q1 nil) delta
      (EmbedResult_Q1 (EvalSpec_Q1 (S_Q1 (cutoff_Q1 delta)) nil)).
Proof.
  intros delta Hpre.
  rewrite EvalSpec_S_Q1.
  rewrite cutoff_Q1_eq_EmbedParams.
  exact (FormalSQLAuxCorrect_Q1_nil delta Hpre).
Qed.
