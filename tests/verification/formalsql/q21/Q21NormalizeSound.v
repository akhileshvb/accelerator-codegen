(* Q21NormalizeSound — Q1-style query-specific NormalizeSound for Q21.

   Architecture: see Q21_STRATEGY.md.
   Recognition (normalize_q21_source / shared) remains recognition-only (Qed).
   Shared ScanCorrespond / ValueCorrespond / SlotMap are available helpers;
   they are NOT required as a complete operator morphism stack.

   Fail-closed target (no Eval None → []):

     Theorem NormalizeSound_Q21 :
       forall db nation_name rows,
         Preconditions_Q21 db nation_name ->
         Eval_Spec_Q21 nation_name db = Some rows ->
         exists outs D,
           Embed_Q21_Result rows = Some outs /\
           EmbedDB_Q21 db = Some D /\
           FormalSQL_exec D (qhat_Q21 nation_name) outs.
*)

From Stdlib Require Import List String.
Import ListNotations.
Open Scope string_scope.

From FormalSQLLogical Require Import SqlTypes Schema RelNode Eval.
From LogosGenerated Require Import Queries.
Require Import Q21Schema.
Require Import Q21LogicalSpecTree.
Require Import Q21LogicalNormalize.
Require Import Q21Qhat.
Require Import Q21FormalSQLProof.

(** * Recognition ≠ soundness *)

Lemma NormalizeRecognition_Q21_source :
  normalize_q21 Queries.source_query_expr = Some LogicalSpec_Q21_rel.
Proof. exact normalize_q21_source. Qed.

Lemma NormalizeRecognition_Q21_shared :
  normalize_q21 Queries.shared_query_expr_0 = Some LogicalSpec_Q21_rel.
Proof. exact normalize_q21_shared. Qed.

Lemma NormalizeRecognition_Q21_catalog_qhat :
  normalize_q21 (qhat_Q21 catalog_nation_name) = Some LogicalSpec_Q21_rel.
Proof.
  rewrite qhat_Q21_catalog_instance.
  exact normalize_q21_source.
Qed.

(** * RelNode empty half (local sanity only; not the public Pre domain) *)

Lemma NormalizeSound_Q21_relnode_empty :
  forall nation_name,
    Eval_Spec_Q21 nation_name db_empty_Q21 = Some [] /\
    Embed_Q21_Result [] = Some [].
Proof.
  intros nation_name.
  split.
  - apply Eval_Spec_Q21_empty.
  - apply Embed_Q21_Result_nil.
Qed.

(** * Public fail-closed theorem statement *)

Definition NormalizeSound_Q21_statement : Prop :=
  forall (db : DB) (nation_name : string) (rows : list Row),
    Preconditions_Q21 db nation_name ->
    Eval_Spec_Q21 nation_name db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) outs.

(** Empty-DB specialization (sanity lemma shape only).

    RelNode Eval + Embed_Result are Qed ([NormalizeSound_Q21_relnode_empty]).
    FormalSQL_exec on the empty multi-table witness for the exact Q21
    CrossJoin+EXISTS+Group+Order spine remains open — do not invest in a
    general empty-spine framework; close it only as a small local check. *)

Definition NormalizeSound_Q21_nil_statement : Prop :=
  forall nation_name,
    NationNameOk nation_name ->
    exists D,
      EmbedDB_Q21 db_empty_Q21 = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) nil /\
      Eval_Spec_Q21 nation_name db_empty_Q21 = Some [] /\
      Embed_Q21_Result [] = Some [].

Lemma NormalizeSound_Q21_nil_relnode_half :
  forall nation_name,
    Eval_Spec_Q21 nation_name db_empty_Q21 = Some [] /\
    Embed_Q21_Result [] = Some [].
Proof.
  intros nation_name.
  apply NormalizeSound_Q21_relnode_empty.
Qed.

(** * Slot / layout facts (Q21-specific) *)

Lemma Q21_outer_width_slots :
  outer_width = 11%nat /\
  slot_l1_orderkey = 3%nat /\
  slot_l1_suppkey = 4%nat /\
  slot_r_orderkey = 11%nat /\
  slot_r_suppkey = 12%nat.
Proof. repeat split; reflexivity. Qed.

(** * Status

   NormalizeSound_Q21: NOT Qed (statement tightened; fail-closed).
   First nonempty semantic leaf Qed: EmbedRow_supplier_row_correspond
   (+ nonempty EmbedDB / Preconditions example).
   Next blockers: FormalSQL CrossJoin/filter spine, EXISTS/NOT EXISTS
   ConjList glue, GROUP BY + COUNT star, Project/ORDER BY — query-specific,
   no generic FormalSQL→RelNode compiler. Stage 3 blocked.
*)
