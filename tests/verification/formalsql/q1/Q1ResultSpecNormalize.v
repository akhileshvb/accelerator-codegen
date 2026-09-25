(* Q1ResultSpecNormalize — gate normalize(qhat_Q1 δ) = Some(S_Q1(cutoff δ)). *)

From FormalSQLLib Require Import QuerySpineFacade.
From LogosGenerated Require Import Queries.
From Logos Require Import FormalSQL.TNullSyntax.
From Stdlib Require Import List ZArith.
Import ListNotations.
Open Scope Z_scope.

From FormalSQLResultSpec Require Import QueryResultSpec NormalizeQuery.
Require Import Q1FormalSQLProof.

Lemma extract_delta_q1_filter_at :
  forall delta, extract_delta_from_filter (q1_filter_at delta) = Some delta.
Proof.
  intros delta.
  unfold extract_delta_from_filter, q1_filter_at.
  unfold extract_delta_from_pred_args, extract_delta_from_add_args.
  unfold extract_z_cst_leaf.
  unfold CstZ, AExpr, Constant.
  simpl.
  rewrite Z.opp_involutive.
  reflexivity.
Qed.

Lemma normalize_qhat_Q1 :
  forall delta,
    normalize_query (qhat_Q1 delta) = Some (S_Q1 (cutoff_Q1 delta)).
Proof.
  intros delta.
  unfold normalize_query, qhat_Q1, query_spine_TFGOP.
  unfold q1_having, q1_table.
  cbn -[q1_filter_at extract_delta_from_filter S_Q1 cutoff_Q1].
  rewrite extract_delta_q1_filter_at.
  reflexivity.
Qed.

Lemma normalize_qhat_Q1_catalog :
  normalize_query (qhat_Q1 q1_catalog_delta) = Some (S_Q1 (cutoff_Q1 q1_catalog_delta)).
Proof. apply normalize_qhat_Q1. Qed.

Lemma normalize_qhat_Q1_catalog_source :
  normalize_query Queries.source_query_expr = Some (S_Q1 (cutoff_Q1 90)).
Proof.
  rewrite <- qhat_Q1_catalog_instance.
  apply normalize_qhat_Q1.
Qed.

Lemma cutoff_Q1_eq_EmbedParams :
  forall delta, cutoff_Q1 delta = EmbedParams_Q1 delta.
Proof. reflexivity. Qed.
