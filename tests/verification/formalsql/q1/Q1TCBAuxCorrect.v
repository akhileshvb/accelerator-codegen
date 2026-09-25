(* Q1 TCB-boundary AuxCorrect: D + restated FormalSQLAuxCorrect_Q1.

   Hyps: Pre + Q1NumericSafe + NumericEmbedFaithful.
   A (bucket Safe) is Qed from Pre — not TCB.
   B NumericEmbedFaithful — TCB representation (cells).
   C Q1_GroupsCorrespond — Qed from Pre + Faithful
     ([Q1_GroupsCorrespond_holds] via SortedGroupResultFacade;
      [Q1_GroupBagsPerm] discharged).
   D below — outer embed of group tuples ⇒ final list-eq (Qed). *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueString
  SqlOutcome SqlQuerySyntax SqlQuerySemantics SqlOrder
  OrderedSet Env Interp FiniteBag FiniteSet FTuples Projection.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax.
From FormalSQLLib Require Import OrderRowsFacade.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String List ZArith Lia.
Import ListNotations.
Import Tuple.
Open Scope Z_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence
  Q1AuxCorrectAssembly Q1Obligations Q1FormalSQLSpine
  Q1NumericEmbed Q1GroupsCorrespond.

(** * D: outer dots of group-tuple = EmbedOutRow value list *)

Lemma q1_group_tuple_dot_rf :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o)
      (AttrString "l_returnflag" (StringChar 1)) =
    embed_flag_char (Q1AuxSpec.out_rf o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_ls :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o)
      (AttrString "l_linestatus" (StringChar 1)) =
    embed_flag_char (Q1AuxSpec.out_ls o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_sum_qty :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "sum_qty") =
    embed_decimal2 (Q1AuxSpec.out_sum_qty o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_sum_base :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "sum_base_price") =
    embed_decimal2 (Q1AuxSpec.out_sum_base o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_sum_disc :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "sum_disc_price") =
    embed_decimal_at (Q1AuxSpec.out_sum_disc_price o) 4.
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_sum_charge :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "sum_charge") =
    embed_decimal_at (Q1AuxSpec.out_sum_charge o) 6.
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_avg_qty :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "avg_qty") =
    embed_avg_from_sum_scale2
      (Q1AuxSpec.out_sum_qty o) (Q1AuxSpec.out_count o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_avg_price :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "avg_price") =
    embed_avg_from_sum_scale2
      (Q1AuxSpec.out_sum_base o) (Q1AuxSpec.out_count o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_avg_disc :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrNumeric "avg_disc") =
    embed_avg_from_sum_scale2
      (Q1AuxSpec.out_sum_discount o) (Q1AuxSpec.out_count o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_group_tuple_dot_count :
  forall o,
    dot TNull (q1_group_tuple_of_OutRow o) (AttrInt64 "count_order") =
    NullValues.value_int64_checked (Q1AuxSpec.out_count o).
Proof.
  intro o.
  unfold q1_group_tuple_of_OutRow, project_row, scalar_select_outputs,
    q1_group_select, Queries.scalar_select_list_1, q1_group_values_of_OutRow.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_outer_dot_values_of_group_tuple :
  forall o,
    q1_outer_dot_values (q1_group_tuple_of_OutRow o) =
    [embed_flag_char (Q1AuxSpec.out_rf o)
    ; embed_flag_char (Q1AuxSpec.out_ls o)
    ; embed_decimal2 (Q1AuxSpec.out_sum_qty o)
    ; embed_decimal2 (Q1AuxSpec.out_sum_base o)
    ; embed_decimal_at (Q1AuxSpec.out_sum_disc_price o) 4
    ; embed_decimal_at (Q1AuxSpec.out_sum_charge o) 6
    ; embed_avg_from_sum_scale2
        (Q1AuxSpec.out_sum_qty o) (Q1AuxSpec.out_count o)
    ; embed_avg_from_sum_scale2
        (Q1AuxSpec.out_sum_base o) (Q1AuxSpec.out_count o)
    ; embed_avg_from_sum_scale2
        (Q1AuxSpec.out_sum_discount o) (Q1AuxSpec.out_count o)
    ; NullValues.value_int64_checked (Q1AuxSpec.out_count o)
    ].
Proof.
  intro o.
  unfold q1_outer_dot_values.
  rewrite q1_group_tuple_dot_rf, q1_group_tuple_dot_ls,
    q1_group_tuple_dot_sum_qty, q1_group_tuple_dot_sum_base,
    q1_group_tuple_dot_sum_disc, q1_group_tuple_dot_sum_charge,
    q1_group_tuple_dot_avg_qty, q1_group_tuple_dot_avg_price,
    q1_group_tuple_dot_avg_disc, q1_group_tuple_dot_count.
  reflexivity.
Qed.

Lemma q1_outer_select_snd :
  map snd q1_outer_select = q1_out_attrs.
Proof.
  unfold q1_outer_select, Queries.scalar_select_list_0, q1_out_attrs.
  reflexivity.
Qed.

Lemma q1_outer_of_group_tuple_OutRow :
  forall o,
    project_row q1_outer_select
      (q1_outer_dot_values (q1_group_tuple_of_OutRow o)) =
    EmbedOutRow_Q1 o.
Proof.
  intro o.
  rewrite q1_outer_dot_values_of_group_tuple.
  unfold EmbedOutRow_Q1, q1_outer_select, q1_group_values_of_OutRow,
    embed_outrow_values.
  reflexivity.
Qed.

(** D: ordered group-obs correspondence ⇒ final EmbedResult. *)
Lemma q1_final_rows_eq_of_groups :
  forall db delta,
    Q1_GroupsCorrespond db delta ->
    q1_final_rows_of db delta =
      EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta)).
Proof.
  intros db delta Hgrp.
  unfold q1_final_rows_of, EmbedResult_Q1, Q1_GroupsCorrespond in *.
  rewrite Hgrp.
  rewrite map_map.
  apply map_ext; intro o; apply q1_outer_of_group_tuple_OutRow.
Qed.

(** * Restated AuxCorrect under the TCB boundary *)

Theorem FormalSQLAuxCorrect_Q1 :
  forall db delta,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta))).
Proof.
  intros db delta Hpre Hsafe Hnum.
  apply FormalSQLAuxCorrect_Q1_of_holds; try assumption.
  apply q1_final_rows_eq_of_groups.
  exact (Q1_GroupsCorrespond_holds db delta Hpre Hnum).
Qed.
