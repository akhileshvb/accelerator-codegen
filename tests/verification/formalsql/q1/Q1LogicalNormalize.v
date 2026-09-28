(* Q1LogicalNormalize — compositional normalize(qhat_Q1)=Some(LogicalSpec_Q1). *)
From FormalSQLResultSpec Require Import
  NormalizeCompositional LogicalSpec QueryResultSpec NormalizeQuery.
From FormalSQLLib Require Import QuerySpineFacade.
From LogosGenerated Require Import Queries.
From Logos Require Import FormalSQL.QueryTNullSyntax FormalSQL.TNullSyntax.
Require Import Q1FormalSQLProof.
Require Import Q1ResultSpecNormalize.
From SQLFS Require Import
  ValueInteger Values ValueCore SqlQuerySyntax SqlOrder FTerms ATerms
  GenericInstance.
From Stdlib Require Import ZArith List Bool String Nat Lia.
Import ListNotations.
Import NullValues.
Open Scope Z_scope.
Open Scope string_scope.

Lemma value_eqb_refl : forall v, value_eqb v v = true.
Proof.
  intros v; destruct v; simpl; try reflexivity;
    try (destruct o; simpl; [rewrite Z.eqb_refl|]; reflexivity).
Qed.

Lemma string_typmod_eqb_refl : forall t, string_typmod_eqb t t = true.
Proof.
  intros t; destruct t; simpl; try reflexivity; apply Nat.eqb_refl.
Qed.

Lemma attribute_eqb_refl : forall a, attribute_eqb a a = true.
Proof.
  intros a; destruct a; simpl;
    try (rewrite String.eqb_refl; reflexivity);
    try (rewrite String.eqb_refl, string_typmod_eqb_refl; reflexivity);
    try (rewrite String.eqb_refl, Z.eqb_refl; reflexivity);
    try (rewrite String.eqb_refl, Z.eqb_refl, Z.eqb_refl; reflexivity).
Qed.

Lemma type_eqb_refl : forall t, type_eqb t t = true.
Proof.
  intros t. unfold type_eqb.
  apply (proj2 (TNullTypeEqb_eq t t) eq_refl).
Qed.

Lemma funterm_eqb_refl : forall t, funterm_eqb t t = true.
Proof.
  intros t; destruct t; simpl.
  - apply value_eqb_refl.
  - apply attribute_eqb_refl.
  - reflexivity.
Qed.

Lemma aggregate_function_eqb_refl :
  forall f, aggregate_function_eqb f f = true.
Proof.
  intros f; destruct f; simpl; try reflexivity;
    try (rewrite Z.eqb_refl; reflexivity);
    try (rewrite Z.eqb_refl, Z.eqb_refl; reflexivity).
Qed.

Lemma aggregate_quantifier_eqb_refl :
  forall q, aggregate_quantifier_eqb q q = true.
Proof. intros q; destruct q; reflexivity. Qed.

Lemma scalar_numeric_kind_eqb_refl :
  forall k, scalar_numeric_kind_eqb k k = true.
Proof. intros k; destruct k; reflexivity. Qed.

Lemma scalar_cast_eqb_refl : forall c, scalar_cast_eqb c c = true.
Proof.
  intros c; destruct c; simpl; try reflexivity;
    try (match goal with
         | |- scalar_numeric_source_eqb ?s ?s = true =>
             destruct s; reflexivity
         end).
Qed.

Lemma scalar_timestamp_unit_eqb_refl :
  forall u, scalar_timestamp_unit_eqb u u = true.
Proof. intros u; destruct u; reflexivity. Qed.

Lemma scalar_op_eqb_refl :
  forall op, scalar_op_eqb op op = true.
Proof.
  intros op; destruct op; simpl; try reflexivity;
    try apply scalar_numeric_kind_eqb_refl;
    try apply scalar_cast_eqb_refl;
    try apply scalar_timestamp_unit_eqb_refl.
Qed.

Lemma aggterm_eqb_refl : forall t, aggterm_eqb t t = true.
Proof.
  fix IH 1.
  intros t; destruct t; simpl.
  - apply funterm_eqb_refl.
  - destruct a; simpl.
    + rewrite aggregate_function_eqb_refl, aggregate_quantifier_eqb_refl,
        funterm_eqb_refl; reflexivity.
    + reflexivity.
  - rewrite scalar_op_eqb_refl.
    revert l; fix IHargs 1; intros args; destruct args as [|a tl]; simpl.
    + reflexivity.
    + rewrite (IH a), (IHargs tl); reflexivity.
Qed.

Lemma scalar_expr_eqb_refl :
  forall R (e : @scalar_expr TNull relname R),
    scalar_expr_eqb e e = true.
Proof.
  fix IH 2.
  intros R e; destruct e; simpl;
    try reflexivity;
    try (rewrite type_eqb_refl, aggterm_eqb_refl; reflexivity);
    try (rewrite type_eqb_refl, scalar_op_eqb_refl; rewrite !andb_true_l;
         match goal with
         | args : list (@scalar_expr _ _ ScalarResultValue) |- _ =>
             generalize args; fix IHl 1; intros args';
             destruct args' as [|a tl]; simpl;
               [reflexivity | rewrite (IH _ a), (IHl tl); reflexivity]
         end);
    try (rewrite predicate_eqb_refl; rewrite !andb_true_l;
         match goal with
         | args : list (@scalar_expr _ _ ScalarResultValue) |- _ =>
             generalize args; fix IHl 1; intros args';
             destruct args' as [|a tl]; simpl;
               [reflexivity | rewrite (IH _ a), (IHl tl); reflexivity]
         end);
    try (rewrite type_eqb_refl; rewrite !andb_true_l;
         repeat match goal with
         | H : @scalar_expr _ _ _ |- _ =>
             progress (rewrite (IH _ H); clear H)
         end; reflexivity);
    try (match goal with
         | H : @scalar_expr _ _ _ |- _ => apply (IH _ H)
         end);
    try (match goal with
         | args : list (@scalar_expr _ _ ScalarResultValue) |- _ =>
             generalize args; fix IHl 1; intros args';
             destruct args' as [|a tl]; simpl;
               [reflexivity | rewrite (IH _ a), (IHl tl); reflexivity]
         end);
    try apply type_eqb_refl.
  match goal with
  | args : list (scalar_expr TNull relname ScalarResultBoolean) |- _ =>
      clear - IH args;
      revert args; fix IHl 1; intros args; destruct args as [|a tl]; simpl;
        [reflexivity | rewrite (IH _ a), (IHl tl); reflexivity]
  end.
Qed.

Lemma select_item_eqb_refl :
  forall it, select_item_eqb it it = true.
Proof.
  intros [e a]; unfold select_item_eqb; simpl.
  rewrite scalar_expr_eqb_refl, attribute_eqb_refl; reflexivity.
Qed.

Lemma select_list_eqb_refl :
  forall xs, select_list_eqb xs xs = true.
Proof.
  induction xs as [|x xs IH]; simpl; [reflexivity|].
  rewrite select_item_eqb_refl, IH; reflexivity.
Qed.

Lemma select_list_eqb_0 :
  select_list_eqb Queries.scalar_select_list_0 Queries.scalar_select_list_0 = true.
Proof. apply select_list_eqb_refl. Qed.

Lemma select_list_eqb_1 :
  select_list_eqb Queries.scalar_select_list_1 Queries.scalar_select_list_1 = true.
Proof. apply select_list_eqb_refl. Qed.

Lemma select_list_eqb_2 :
  select_list_eqb Queries.scalar_select_list_2 Queries.scalar_select_list_2 = true.
Proof. apply select_list_eqb_refl. Qed.

Lemma is_q1_filter_at_q1_filter :
  forall delta, is_q1_filter_at (q1_filter_at delta) delta = true.
Proof.
  intros delta.
  unfold is_q1_filter_at, q1_filter_at, q1_filter_at_expected, q1_base_date.
  apply scalar_expr_eqb_refl.
Qed.

Lemma attr_list_eqb_refl :
  forall xs, attr_list_eqb xs xs = true.
Proof.
  induction xs as [|x xs IH]; simpl; [reflexivity|].
  rewrite attribute_eqb_refl, IH; reflexivity.
Qed.

Lemma group_keys_eqb_refl :
  forall xs, group_keys_eqb xs xs = true.
Proof.
  induction xs as [|x xs IH]; simpl; [reflexivity|].
  rewrite scalar_expr_eqb_refl, IH; reflexivity.
Qed.

Lemma sort_key_eqb_refl : forall k, sort_key_eqb k k = true.
Proof.
  intros k; unfold sort_key_eqb.
  destruct (sort_key_direction k); destruct (sort_key_null_direction k);
    rewrite attribute_eqb_refl; reflexivity.
Qed.

Lemma sort_keys_eqb_refl :
  forall xs, sort_keys_eqb xs xs = true.
Proof.
  induction xs as [|x xs IH]; simpl; [reflexivity|].
  rewrite sort_key_eqb_refl, IH; reflexivity.
Qed.

Lemma is_exact_qhat_Q1_ast_qhat :
  forall delta,
    is_exact_qhat_Q1_ast
      q1_outer_select q1_order_keys q1_group_select q1_group_keys
      q1_having q1_inner_select (q1_filter_at delta)
      q1_table_outputs q1_table delta = true.
Proof.
  intros delta.
  unfold is_exact_qhat_Q1_ast.
  unfold q1_outer_select, q1_inner_select, q1_group_select, q1_having.
  unfold table_is_lineitem, q1_table.
  change (String.eqb "lineitem" "lineitem") with true.
  rewrite is_q1_filter_at_q1_filter.
  rewrite select_list_eqb_2, select_list_eqb_1, select_list_eqb_0.
  unfold q1_group_keys, q1_order_keys, q1_table_outputs.
  unfold q1_expected_group_keys, q1_expected_order_keys, q1_expected_table_outputs.
  unfold Queries.scalar_expr_predicate_0.
  rewrite attr_list_eqb_refl.
  rewrite group_keys_eqb_refl.
  rewrite sort_keys_eqb_refl.
  reflexivity.
Qed.

Lemma normalize_q1_qhat :
  forall delta,
    normalize_q1 (qhat_Q1 delta) = Some LogicalSpec_Q1.
Proof.
  intros delta.
  unfold normalize_q1.
  unfold qhat_Q1, query_spine_TFGOP.
  cbn -[extract_delta_from_filter is_exact_qhat_Q1_ast LogicalSpec_Q1].
  rewrite extract_delta_q1_filter_at.
  rewrite is_exact_qhat_Q1_ast_qhat.
  reflexivity.
Qed.

Lemma normalize_q1_catalog :
  normalize_q1 Queries.source_query_expr = Some LogicalSpec_Q1.
Proof.
  rewrite <- qhat_Q1_catalog_instance.
  apply normalize_q1_qhat.
Qed.

(** Witness: distinct int32 constants compare unequal (exactness). *)
Lemma value_eqb_int32_distinct :
  value_eqb (Value_int32 (int32_checked 1)) (Value_int32 (int32_checked 2)) = false.
Proof.
  assert (H1 : exists i, int32_checked 1 = Some i /\ int32_value i = 1).
  { unfold int32_checked.
    destruct (Z.leb_spec0 int32_min 1) as [Hlo|Hlo]; [|exfalso; unfold int32_min in Hlo; lia].
    destruct (Z.leb_spec0 1 int32_max) as [Hhi|Hhi]; [|exfalso; unfold int32_max in Hhi; lia].
    exists (Int32 1 (conj Hlo Hhi)); split; reflexivity. }
  assert (H2 : exists i, int32_checked 2 = Some i /\ int32_value i = 2).
  { unfold int32_checked.
    destruct (Z.leb_spec0 int32_min 2) as [Hlo|Hlo]; [|exfalso; unfold int32_min in Hlo; lia].
    destruct (Z.leb_spec0 2 int32_max) as [Hhi|Hhi]; [|exfalso; unfold int32_max in Hhi; lia].
    exists (Int32 2 (conj Hlo Hhi)); split; reflexivity. }
  destruct H1 as [i1 [E1 V1]], H2 as [i2 [E2 V2]].
  unfold value_eqb; rewrite E1, E2; simpl; rewrite V1, V2; reflexivity.
Qed.
