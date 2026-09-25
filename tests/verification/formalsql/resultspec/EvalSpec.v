(* EvalSpec — result-only evaluation of QueryResultSpec_Q1.
   For the Q1 TFGOP fragment, EvalSpec(S_Q1(c), D) is defined as the
   existing result-oriented AuxAnswerSpec at cutoff c (filter→group→agg→order).
   This is the Rocq-side EvalSpec used by NormalizeSound; it contains no
   accelerator day-partition Inv state in the theorem statement — FoldSpec is
   only an implementation of the Spec fold. *)

From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import QueryResultSpec.
From JetStreamFS Require Import Q1AuxSpec.

(** EvalSpec for a Q1 Spec descriptor on an abstract row bag [db]. *)
Definition EvalSpec_Q1 (s : QueryResultSpec_Q1) (db : list Q1AuxSpec.Row)
  : list Q1AuxSpec.OutRow :=
  Q1AuxSpec.AuxAnswerSpec db (qrs_cutoff s).

Lemma EvalSpec_S_Q1 :
  forall cutoff db,
    EvalSpec_Q1 (S_Q1 cutoff) db = Q1AuxSpec.AuxAnswerSpec db cutoff.
Proof. reflexivity. Qed.
