(* EvalSpec — LEGACY Q1 ResultSpec evaluator (migration/reference scaffolding).

   EvalSpec_Q1(S_Q1(c), D) aliases AuxAnswerSpec. It is NOT the public Spec
   endpoint after Phase 3/4 — use Eval_Spec(LogicalSpec_Q1, params, db) in
   EvalLogicalSpec.v. Kept only so migration lemmas can rewrite through
   AuxAnswerSpec / EvalSpec_S_Q1. *)

From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import QueryResultSpec.
From JetStreamFS Require Import Q1AuxSpec.

(** Legacy EvalSpec for tag-era Q1 Spec descriptor (scaffolding). *)
Definition EvalSpec_Q1 (s : QueryResultSpec_Q1) (db : list Q1AuxSpec.Row)
  : list Q1AuxSpec.OutRow :=
  Q1AuxSpec.AuxAnswerSpec db (qrs_cutoff s).

Lemma EvalSpec_S_Q1 :
  forall cutoff db,
    EvalSpec_Q1 (S_Q1 cutoff) db = Q1AuxSpec.AuxAnswerSpec db cutoff.
Proof. reflexivity. Qed.
