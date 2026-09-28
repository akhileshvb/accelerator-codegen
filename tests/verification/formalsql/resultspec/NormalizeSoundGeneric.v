(* NormalizeSoundGeneric — Eval_Spec bridges for Q1.
   NormalizeSound_generic is a thin package over FormalSQLSpecCorrect_Q1.
   NormalizeSound_Q1 is a corollary.
   Migration Eval_Spec∘Q1Adapt = AuxAnswerSpec is in q1/Q1EvalMigration.v. *)

From Stdlib Require Import List ZArith String.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import
  LogicalSpec NormalizeCompositional EvalLogicalSpec QueryResultSpec EvalSpec.
From JetStreamFS Require Import Q1AuxSpec.

(** Re-export LogicalSpec ↔ S_Q1 bridge. *)
Lemma LogicalSpec_Q1_to_S_Q1 :
  forall c, LogicalSpec_to_Q1 LogicalSpec_Q1 (params_of_cutoff c) = Some (S_Q1 c).
Proof. intros; reflexivity. Qed.

(** IR unfolding: Eval_Spec_Q1_rows = Q1_IR_eval (no Q1 fast-path). *)
Lemma Eval_Spec_Q1_rows_eq_IR :
  forall c db,
    Eval_Spec_Q1_rows LogicalSpec_Q1 (params_of_cutoff c) db =
    Q1_IR_eval c db.
Proof. intros; apply Eval_Spec_Q1_eq_IR. Qed.
