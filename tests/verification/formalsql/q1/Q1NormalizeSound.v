(* Q1NormalizeSound — public SpecCorrect / NormalizeSound for Q1.

   Canonical evaluation endpoint (Phase RelNode):
     Eval_Spec_Q1_rows_rel = Q1Adapt ∘ Eval_Spec(LogicalSpec_Q1_rel)
   Flat LogicalSpec_Q1 / Eval_Spec_flat remain migration scaffolding only;
   observational equality is Qed in Q1RelNodeBridge.

   NormalizeSound_generic (Q1 family) is the canonical theorem; NormalizeSound_Q1
   is a corollary. FormalSQLSpecCorrect_Q1 is the public SpecCorrect name.
   FormalSQLAuxCorrect_Q1 (Q1TCBAuxCorrect) remains the AuxAnswerSpec reference.

   [Q1ByteKeys] is discharged from [Preconditions_Q1] — not an extra public premise.

   Scope (honest):
   - Proved for [qhat_Q1 delta] (supported family normalize accepts for Q1).
   - With [normalize_q1_qhat]: normalize_q1(qhat)=Some(LogicalSpec_Q1) ∧ sound
     (normalize still emits the flat template as the recognized form; evaluation
     is via the RelNode tree LogicalSpec_Q1_rel).
   - Residuals: NumericEmbedFaithful [TCB]
*)

From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import
  QueryResultSpec NormalizeQuery EvalSpec LogicalSpec NormalizeCompositional
  EvalLogicalSpec NormalizeSoundGeneric.
From JetStreamFS Require Import Q1AuxSpec.

Require Import Q1FormalSQLProof.
Require Import Q1NumericEmbed.
Require Import Q1GroupsCorrespond.
Require Import Q1EvalMigration.
Require Import Q1TCBAuxCorrect.
Export Q1TCBAuxCorrect.
Require Import Q1ResultSpecNormalize.
Require Import Q1LogicalNormalize.
Require Import Q1Adapt.
Require Import Q1RelNodeBridge.

(** RelNode Eval_Spec OutRow view. Under [Preconditions_Q1], evaluation always
    returns [Some _]; the [None -> nil] arm is only for total typing. *)
Definition Eval_Spec_Q1_outs (c : Z) (db : list Q1AuxSpec.Row)
  : list Q1AuxSpec.OutRow :=
  match Eval_Spec_Q1_rows_rel c db with
  | Some outs => outs
  | None => nil
  end.

Lemma Eval_Spec_Q1_outs_eq_Aux :
  forall c db delta,
    Preconditions_Q1 db delta ->
    Eval_Spec_Q1_outs c db = Q1AuxSpec.AuxAnswerSpec db c.
Proof.
  intros c db delta Hpre.
  unfold Eval_Spec_Q1_outs.
  rewrite (Eval_Spec_Q1_rows_rel_eq_AuxAnswerSpec_pre c db delta Hpre).
  reflexivity.
Qed.

Lemma Eval_Spec_Q1_outs_Some :
  forall c db delta,
    Preconditions_Q1 db delta ->
    Eval_Spec_Q1_rows_rel c db =
    Some (Eval_Spec_Q1_outs c db).
Proof.
  intros c db delta Hpre.
  unfold Eval_Spec_Q1_outs.
  rewrite (Eval_Spec_Q1_rows_rel_eq_AuxAnswerSpec_pre c db delta Hpre).
  reflexivity.
Qed.

(** Public SQL-path SpecCorrect: FormalSQL = EmbedResult(RelNode Eval_Spec ∘ Q1Adapt).
    Premises unchanged; Q1ByteKeys not exposed. *)
Theorem FormalSQLSpecCorrect_Q1 :
  forall db delta,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (EmbedParams_Q1 delta) db)).
Proof.
  intros db delta Hpre Hsafe Hnum.
  rewrite (Eval_Spec_Q1_outs_eq_Aux _ _ _ Hpre).
  exact (FormalSQLAuxCorrect_Q1 db delta Hpre Hsafe Hnum).
Qed.

(** * NormalizeSound_generic — Q1 family (fragment instance) *)

Theorem NormalizeSound_generic :
  forall (db : list Q1AuxSpec.Row) (delta : Z),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    normalize_q1 (qhat_Q1 delta) = Some LogicalSpec_Q1 /\
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db)).
Proof.
  intros db delta Hpre Hsafe Hnum.
  split.
  - apply normalize_q1_qhat.
  - rewrite cutoff_Q1_eq_EmbedParams.
    exact (FormalSQLSpecCorrect_Q1 db delta Hpre Hsafe Hnum).
Qed.

(** Corollary: legacy NormalizeSound_Q1 name (same RelNode Eval_Spec target). *)
Theorem NormalizeSound_Q1 :
  forall (db : list Q1AuxSpec.Row) (delta : Z),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db)).
Proof.
  intros db delta Hpre Hsafe Hnum.
  exact (proj2 (NormalizeSound_generic db delta Hpre Hsafe Hnum)).
Qed.

Theorem Normalize_and_Sound_Q1 :
  forall (db : list Q1AuxSpec.Row) (delta : Z),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    normalize_query (qhat_Q1 delta) = Some (S_Q1 (cutoff_Q1 delta)) /\
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db)).
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
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) nil)).
Proof.
  intros delta Hpre.
  rewrite (Eval_Spec_Q1_outs_eq_Aux _ _ _ Hpre).
  rewrite cutoff_Q1_eq_EmbedParams.
  exact (FormalSQLAuxCorrect_Q1_nil delta Hpre).
Qed.
