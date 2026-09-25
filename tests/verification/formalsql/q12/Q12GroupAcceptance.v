(* Q12GroupAcceptance — GroupBagOk for scalar_select_list_1 (SUM Int32).

   Per-bucket shipmode key + SUM($f1)/SUM($f2) exactness from
   AggregateLeafExactFacade. Consumed by Q12JoinSpine GroupBagOk_holds. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueInteger
  ValueTemporal ValueString FiniteSet FTuples OrderedSet FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Interp ATerms FTerms Formula ValuePredicates Projection
  SqlListFacts.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.StringTemporalFacts
  FormalSQL.ScalarPredicateFacts FormalSQL.QueryCardinality
  FormalSQL.AggregateRuntimeFacts FormalSQL.NumericFacts.
From FormalSQLLib Require Import
  FilterAcceptanceFacade ProjectExactFacade CaseExactFacade
  GroupsExactFacade GroupBagExactFacade AggregateLeafExactFacade
  TemporalPredAcceptanceFacade NumericCallExactFacade.
From FormalSQLResultSpec Require Import
  QueryResultSpecQ12 Q12EvalSpec Q12SemanticBridge.
From LogosGenerated Require Import Schema Queries.
From Stdlib Require Import String List ZArith Bool Lia Permutation.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q12Catalog.
Require Import Q12NormalizeSound.
Require Import Q12EmbedDots.
Require Import Q12WhereAcceptance.
Require Import Q12CaseAcceptance.

Import Q12EvalSpec.

(** Match JoinSpine [q12_group_terms] (DotString shipmode). *)
Definition q12_gterms : list (aggterm TNull) :=
  [DotString "l_shipmode" (StringChar 10)].

Lemma q12_gterms_nonempty : q12_gterms <> nil.
Proof. unfold q12_gterms; discriminate. Qed.

(** * Group select attributes *)

Definition q12_attr_shipmode : attribute TNull :=
  AttrString "l_shipmode" (StringChar 10).
Definition q12_attr_f1 : attribute TNull := AttrInt32 "$f1".
Definition q12_attr_f2 : attribute TNull := AttrInt32 "$f2".

Definition q12_group_env (group : list (tuple TNull)) : Env.env TNull :=
  Env.env_g TNull nil (@Env.Group_By TNull q12_gterms) group.

Definition q12_group_select_values (group : list (tuple TNull))
  : list NullValues.value :=
  [ Interp.interp_dot TNull (q12_group_env group) q12_attr_shipmode
  ; NullValues.interp_sum_int32_as_int64
      (map (fun row => dot TNull row q12_attr_f1) group)
  ; NullValues.interp_sum_int32_as_int64
      (map (fun row => dot TNull row q12_attr_f2) group)
  ].

Definition q12_group_row_labels_ok (row : tuple TNull) : Prop :=
  q12_attr_shipmode inS labels TNull row /\
  q12_attr_f1 inS labels TNull row /\
  q12_attr_f2 inS labels TNull row.

Definition q12_group_bucket_safe (group : list (tuple TNull)) : Prop :=
  NullValues.sum_int32_runtime_error
    (map (fun row => dot TNull row q12_attr_f1) group) = None /\
  NullValues.sum_int32_runtime_error
    (map (fun row => dot TNull row q12_attr_f2) group) = None.

Lemma q12_project_row_labels :
  forall select_list values,
    labels TNull (@project_row TNull relname select_list values) =S=
    Fset.mk_set (A TNull) (scalar_select_outputs select_list).
Proof.
  intros select_list values.
  unfold project_row, scalar_select_outputs.
  apply labels_mk_tuple.
Qed.

Lemma q12_attr_inS_project_row :
  forall select_list values attr,
    attr inS Fset.mk_set (A TNull) (scalar_select_outputs select_list) ->
    attr inS labels TNull (@project_row TNull relname select_list values).
Proof.
  intros select_list values attr Hin.
  rewrite (Fset.mem_eq_2 _ _ _ (q12_project_row_labels select_list values)).
  exact Hin.
Qed.

Lemma q12_case_select_outputs_shipmode :
  q12_attr_shipmode inS
    Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_2).
Proof.
  unfold q12_attr_shipmode, scalar_select_outputs, Queries.scalar_select_list_2.
  vm_compute; reflexivity.
Qed.

Lemma q12_case_select_outputs_f1 :
  q12_attr_f1 inS
    Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_2).
Proof.
  unfold q12_attr_f1, scalar_select_outputs, Queries.scalar_select_list_2.
  vm_compute; reflexivity.
Qed.

Lemma q12_case_select_outputs_f2 :
  q12_attr_f2 inS
    Fset.mk_set (A TNull) (scalar_select_outputs Queries.scalar_select_list_2).
Proof.
  unfold q12_attr_f2, scalar_select_outputs, Queries.scalar_select_list_2.
  vm_compute; reflexivity.
Qed.

Lemma q12_pre_group_row_labels_ok :
  forall o l,
    q12_group_row_labels_ok (q12_pre_group_row o l).
Proof.
  intros o l.
  unfold q12_group_row_labels_ok, q12_pre_group_row.
  split; [|split].
  - apply q12_attr_inS_project_row; exact q12_case_select_outputs_shipmode.
  - apply q12_attr_inS_project_row; exact q12_case_select_outputs_f1.
  - apply q12_attr_inS_project_row; exact q12_case_select_outputs_f2.
Qed.

Lemma q12_pre_group_rows_one_order_labels :
  forall o lines,
    Forall q12_group_row_labels_ok
      (map (fun l => q12_pre_group_row o l)
        (List.filter (fun l => SpecPairKeep o l) lines)).
Proof.
  intros o lines.
  induction lines as [|l ls IHl]; cbn [filter map]; [constructor|].
  destruct (SpecPairKeep o l); cbn [map].
  - constructor; [apply q12_pre_group_row_labels_ok|exact IHl].
  - exact IHl.
Qed.

Lemma q12_pre_group_rows_labels_Forall :
  forall orders lines,
    Forall q12_group_row_labels_ok (q12_pre_group_rows orders lines).
Proof.
  intros orders lines.
  unfold q12_pre_group_rows.
  induction orders as [|o os IH]; cbn [flat_map]; [constructor|].
  apply Forall_app; split.
  - apply q12_pre_group_rows_one_order_labels.
  - exact IH.
Qed.

(** * Case-bit dots on pre-group rows *)

Lemma q12_pre_group_row_dot_f1 :
  forall o l,
    dot TNull (q12_pre_group_row o l) q12_attr_f1 =
    q12_case_bit (q12_priority_is_high (o_priority o)).
Proof.
  intros o l.
  unfold q12_pre_group_row, q12_case_select_values, q12_attr_f1,
    project_row, scalar_select_outputs, Queries.scalar_select_list_2.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q12_pre_group_row_dot_f2 :
  forall o l,
    dot TNull (q12_pre_group_row o l) q12_attr_f2 =
    q12_case_bit (negb (q12_priority_is_high (o_priority o))).
Proof.
  intros o l.
  unfold q12_pre_group_row, q12_case_select_values, q12_attr_f2,
    project_row, scalar_select_outputs, Queries.scalar_select_list_2.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q12_pre_group_row_dot_shipmode :
  forall o l,
    dot TNull (q12_pre_group_row o l) q12_attr_shipmode =
    embed_string_char10 (l_shipmode l).
Proof.
  intros o l.
  unfold q12_pre_group_row, q12_case_select_values, q12_attr_shipmode,
    project_row, scalar_select_outputs, Queries.scalar_select_list_2.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

(** * Int32 0/1 helpers (bucket_safe discharge deferred to SpecView wiring) *)

Lemma int32_checked_0_1_some :
  forall b,
    exists i, int32_checked (q12_case_bit_z b) = Some i /\
      (int32_value i = 0 \/ int32_value i = 1).
Proof.
  intro b; unfold q12_case_bit_z; destruct b; unfold int32_checked.
  - destruct (Z.leb_spec0 int32_min 1) as [Hlo|]; [|exfalso; unfold int32_min in *; lia].
    destruct (Z.leb_spec0 1 int32_max) as [Hhi|]; [|exfalso; unfold int32_max in *; lia].
    exists (Int32 1 (conj Hlo Hhi)); split; [reflexivity|right; reflexivity].
  - destruct (Z.leb_spec0 int32_min 0) as [Hlo|]; [|exfalso; unfold int32_min in *; lia].
    destruct (Z.leb_spec0 0 int32_max) as [Hhi|]; [|exfalso; unfold int32_max in *; lia].
    exists (Int32 0 (conj Hlo Hhi)); split; [reflexivity|left; reflexivity].
Qed.

Lemma q12_case_bit_is_int32 :
  forall b, NullValues.is_int32_value (q12_case_bit b) = true.
Proof.
  intro b.
  unfold q12_case_bit.
  destruct (int32_checked_0_1_some b) as [i [Hi _]].
  rewrite Hi; reflexivity.
Qed.

Lemma q12_group_row_labels_Forall_attr :
  forall attr group,
    Forall q12_group_row_labels_ok group ->
    (forall row, q12_group_row_labels_ok row -> attr inS labels TNull row) ->
    Forall (fun row => attr inS labels TNull row) group.
Proof.
  intros attr group Hlab Hattr.
  eapply Forall_impl; [|exact Hlab].
  intros row Hrow; apply Hattr; exact Hrow.
Qed.

(** * Group select-list exactness (SUM Int32 leaves) *)

Lemma q12_group_select_list_exact :
  forall D sched group,
    group <> nil ->
    Forall q12_group_row_labels_ok group ->
    q12_group_bucket_safe group ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (q12_group_env group) (map fst Queries.scalar_select_list_1)
      (q12_group_select_values group).
Proof.
  intros D sched group Hne Hlab Hsafe.
  destruct Hsafe as [Hsum1 Hsum2].
  assert (Hf1 :
    Forall (fun row => q12_attr_f1 inS labels TNull row) group)
    by (eapply q12_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  assert (Hf2 :
    Forall (fun row => q12_attr_f2 inS labels TNull row) group)
    by (eapply q12_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  unfold Queries.scalar_select_list_1, q12_group_select_values, q12_group_env.
  cbn [map fst].
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull D) (@_instance TNull D)
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched)).
  - apply tnull_dot_string_exact; reflexivity.
  - change (AAggregate AggregateSumInt32 AggregateAll
      (Dot (AttrInt32 "$f1")))
      with (tnull_sum_int32_dot_term q12_attr_f1).
    eapply tnull_sum_int32_dot_leaf_exact; eassumption.
  - change (AAggregate AggregateSumInt32 AggregateAll
      (Dot (AttrInt32 "$f2")))
      with (tnull_sum_int32_dot_term q12_attr_f2).
    eapply tnull_sum_int32_dot_leaf_exact; eassumption.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
Qed.

Lemma q12_group_select_aggregate_runtime_none :
  forall group,
    group <> nil ->
    Forall q12_group_row_labels_ok group ->
    q12_group_bucket_safe group ->
    @eval_scalar_select_aggregate_runtime_error TNull relname
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (q12_group_env group) Queries.scalar_select_list_1 = None.
Proof.
  intros group Hne Hlab Hsafe.
  destruct Hsafe as [Hsum1 Hsum2].
  assert (Hf1 :
    Forall (fun row => q12_attr_f1 inS labels TNull row) group)
    by (eapply q12_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  assert (Hf2 :
    Forall (fun row => q12_attr_f2 inS labels TNull row) group)
    by (eapply q12_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  unfold Queries.scalar_select_list_1, q12_group_env.
  unfold eval_scalar_select_aggregate_runtime_error.
  apply (proj2 (first_runtime_error_none_iff _ _ _)).
  constructor; [simpl fst; reflexivity|].
  constructor; [|constructor; [|constructor]].
  - simpl fst.
    change (AAggregate AggregateSumInt32 AggregateAll
      (Dot (AttrInt32 "$f1")))
      with (tnull_sum_int32_dot_term q12_attr_f1).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_sum_int32_dot_aggregate_runtime_exact
      q12_gterms group q12_attr_f1 Hne Hf1); exact Hsum1.
  - simpl fst.
    change (AAggregate AggregateSumInt32 AggregateAll
      (Dot (AttrInt32 "$f2")))
      with (tnull_sum_int32_dot_term q12_attr_f2).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_sum_int32_dot_aggregate_runtime_exact
      q12_gterms group q12_attr_f2 Hne Hf2); exact Hsum2.
Qed.

Lemma q12_group_filter_map_exact :
  forall D sched group,
    group <> nil ->
    Forall q12_group_row_labels_ok group ->
    q12_group_bucket_safe group ->
    @group_filter_map_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil Queries.scalar_select_list_1 q12_gterms
      Queries.scalar_expr_predicate_0
      (fun _ => true) q12_group_select_values group.
Proof.
  intros D sched group Hne Hlab Hsafe.
  unfold group_filter_map_exact_at, Queries.scalar_expr_predicate_0.
  split.
  - apply q12_group_select_aggregate_runtime_none; assumption.
  - split.
    + reflexivity.
    + split.
      * eapply (@tnull_having_true_acceptance_exact
          (@_basesort TNull D) (@_instance TNull D) sched).
      * apply q12_group_select_list_exact; assumption.
Qed.


(** * bucket_safe for CASE 0/1 Int32 columns *)

Lemma filter_length_le :
  forall {A : Type} (xs : list A) (p : A -> bool),
    (List.length (List.filter p xs) <= List.length xs)%nat.
Proof.
  intros A xs p; induction xs as [|x xs IH]; [exact (le_n 0)|].
  cbn [filter]; destruct (p x); cbn [List.length].
  - apply le_n_S; exact IH.
  - apply le_S; exact IH.
Qed.

Lemma fold_left_case_bits_count_from_ga :
  forall bits acc,
    fold_left (fun a n => a + int32_value n)
      (NullValues.int32_values (map q12_case_bit bits)) acc =
    acc + Z.of_nat (List.length (List.filter (fun b : bool => b) bits)).
Proof.
  induction bits as [|b bs IH]; intros acc; [cbn; lia|].
  destruct b.
  - pose proof (int32_checked_0_1_some true) as [i [Hi Hval]].
    simpl map.
    replace (q12_case_bit true) with (NullValues.Value_int32 (Some i))
      by (unfold q12_case_bit; rewrite Hi; reflexivity).
    simpl NullValues.int32_values; simpl fold_left.
    destruct Hval as [Hz|Ho].
    + unfold q12_case_bit_z, int32_checked in Hi.
      destruct (Z.leb_spec0 int32_min 1); [|exfalso; unfold int32_min in *; lia].
      destruct (Z.leb_spec0 1 int32_max); [|exfalso; unfold int32_max in *; lia].
      inversion Hi; subst i; simpl in Hz; lia.
    + simpl filter; simpl List.length; rewrite Ho, (IH (acc + 1)); lia.
  - pose proof (int32_checked_0_1_some false) as [i [Hi Hval]].
    simpl map.
    replace (q12_case_bit false) with (NullValues.Value_int32 (Some i))
      by (unfold q12_case_bit; rewrite Hi; reflexivity).
    simpl NullValues.int32_values; simpl fold_left.
    destruct Hval as [Hz|Ho].
    + simpl filter; simpl List.length; rewrite Hz, (IH (acc + 0)); lia.
    + unfold q12_case_bit_z, int32_checked in Hi.
      destruct (Z.leb_spec0 int32_min 0); [|exfalso; unfold int32_min in *; lia].
      destruct (Z.leb_spec0 0 int32_max); [|exfalso; unfold int32_max in *; lia].
      inversion Hi; subst i; simpl in Ho; lia.
Qed.

Lemma sum_int32_runtime_error_case_bits_none :
  forall bits,
    Z.of_nat (List.length bits) <= int64_max ->
    NullValues.sum_int32_runtime_error (map q12_case_bit bits) = None.
Proof.
  intros bits Hlen.
  unfold NullValues.sum_int32_runtime_error.
  assert (Hall : forallb NullValues.is_int32_value (map q12_case_bit bits) = true).
  {
    clear Hlen; induction bits as [|b bs IH]; [reflexivity|].
    cbn [map forallb]; rewrite (q12_case_bit_is_int32 b); exact IH.
  }
  rewrite Hall.
  destruct (NullValues.int32_values (map q12_case_bit bits)) as [|i is] eqn:Hv.
  - reflexivity.
  - unfold NullValues.int64_result_runtime_error.
    assert (Hbnd :
      0 <=
        fold_left (fun acc next => acc + int32_value next)
          (i :: is) 0 <=
      Z.of_nat (List.length bits)).
    {
      rewrite <- Hv.
      rewrite (fold_left_case_bits_count_from_ga bits 0).
      pose proof (filter_length_le bits (fun b : bool => b)); lia.
    }
    unfold int64_checked.
    destruct (Z.leb_spec0 int64_min
      (fold_left (fun acc next => acc + int32_value next) (i :: is) 0))
      as [Hlo|]; [|exfalso; unfold int64_min in *; lia].
    destruct (Z.leb_spec0
      (fold_left (fun acc next => acc + int32_value next) (i :: is) 0)
      int64_max) as [Hhi|]; [|exfalso; lia].
    reflexivity.
Qed.
