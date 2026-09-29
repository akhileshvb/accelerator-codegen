(* Q1AllOutcomes — every possible FormalSQL outcome of q̂_Q1 is the SAME success.

   NormalizeSound_generic shows the embedded accelerator result is ONE possible
   outcome of q̂ (FormalSQL_exec is existential over Boolean schedules and over
   bag / ORDER BY representatives).  This file closes the gap: under the same
   premises, every outcome under every schedule equals [SqlSuccess outs].

   Route (generic inversion lemmas in lib/PossibleOutcomeDeterminism.v):
     Table / WHERE                  outcomes_bag   (permut mod =t=)
     inner Project                  outcomes_perm  (Stdlib Permutation)
     GROUP BY rf, ls + aggregates   outcomes_bag, with
       - per-bucket overflow safety for the groups of ANY representative
         (Part 2: the Q1FormalSQLSpine proofs restated over [q1_sub_group]);
       - SUM / AVG independent of in-group row order via the existing
         [NumericEmbedFaithful] premise (sums are embeds of Z folds);
     ORDER BY + final Project       outcomes_order_project + pod_order_unique
       (q1_tuple_le is total, transitive and antisymmetric on the group rows).

   Results:
     q1_ao_filter / q1_ao_inner            WHERE and inner Project outcomes
     q1_group_bucket_safe_of_sub           overflow safety, any sub-bag
     q1_group_values_respect               group values ignore group rep
     q1_ao_group                           Group stage outcomes (bag)
     Q1_qhat_outcomes_unique               any two outcomes of q̂ are equal
                                           (needs only Pre + NumericEmbedFaithful)
     NormalizeSound_Q1_all_outcomes        forall-outcomes NormalizeSound_generic
     NormalizeSound_Q1_catalog_all_outcomes  delta = 90 / catalog query

   No new premises beyond NormalizeSound_generic (Preconditions_Q1,
   Q1NumericSafe, NumericEmbedFaithful).  Fully proved; no new axioms. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueTemporal
  ValueNumericTypmod SchemaConstraints ValueString
  SqlOutcome SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics
  SqlOrder SqlListFacts FiniteBag FiniteCollection FiniteSet FTuples Bool3
  OrderedSet Env Projection Interp ListSort.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.AggregateRuntimeFacts
  FormalSQL.QueryCardinality FormalSQL.NumericRegroupFacts
  FormalSQL.GroupingRewriteFacts FormalSQL.ProofAgentFacade.
From FormalSQLLib Require Import
  QuerySpineFacade FilterAcceptanceFacade ProjectExactFacade
  OrderRowsFacade GroupBagExactFacade GroupsExactFacade
  AggregateLeafExactFacade TemporalPredAcceptanceFacade
  NumericCallExactFacade NumericScaledFits NumericPackagingFacade
  KeyedSortFacade ListNoDupFacade NumericFoldFacade SortedGroupResultFacade
  PossibleOutcomeDeterminism.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String List ZArith Lia Permutation Ascii
  Sorting.Sorted Bool NArith.
Import ListNotations.
Import Tuple.
Open Scope Z_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence
  Q1AuxCorrectAssembly Q1Obligations Q1FormalSQLSpine Q1NumericEmbed
  Q1GroupsCorrespond Q1TCBAuxCorrect Q1NormalizeSound.
From FormalSQLResultSpec Require Import
  QueryResultSpec NormalizeQuery EvalSpec LogicalSpec NormalizeCompositional
  EvalLogicalSpec NormalizeSoundGeneric.
Require Import Q1EvalMigration Q1ResultSpecNormalize Q1LogicalNormalize Q1Adapt
  Q1RelNodeBridge.

(** * Part 1: Table, WHERE, inner Project *)

Lemma q1_teq_interp_dot :
  forall x t a v,
    Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    a inS labels TNull (EmbedRow_Q1 x) ->
    dot TNull (EmbedRow_Q1 x) a = v ->
    Interp.interp_dot TNull (Env.env_t TNull nil t) a = v.
Proof.
  intros x t a v Heq Hin Hv.
  rewrite (interp_dot_env_t nil t a (pod_teq_inS _ _ _ Heq Hin)).
  rewrite (pod_teq_dot _ _ _ Heq Hin); exact Hv.
Qed.

Section Q1Leaves.
Variable db : list Q1AuxSpec.Row.
Variable sched : boolean_site -> boolean_evaluation_order.

Local Abbreviation vexact :=
  (@scalar_value_expr_exact_at TNull relname
    (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched).

Lemma q1_dot_price_exact_teq :
  forall x t, Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    vexact (Env.env_t TNull nil t)
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.price x) 2))).
Proof.
  intros x t Heq; apply tnull_dot_decimal_exact.
  eapply q1_teq_interp_dot; [exact Heq|
    apply EmbedRow_attr_inS_generic, lineitem_attr_inS_price|apply EmbedRow_dot_price].
Qed.

Lemma q1_dot_disc_exact_teq :
  forall x t, Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    vexact (Env.env_t TNull nil t)
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.disc x) 2))).
Proof.
  intros x t Heq; apply tnull_dot_decimal_exact.
  eapply q1_teq_interp_dot; [exact Heq|
    apply EmbedRow_attr_inS_generic, lineitem_attr_inS_disc|apply EmbedRow_dot_disc].
Qed.

Lemma q1_dot_tax_exact_teq :
  forall x t, Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    vexact (Env.env_t TNull nil t)
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.tax x) 2))).
Proof.
  intros x t Heq; apply tnull_dot_decimal_exact.
  eapply q1_teq_interp_dot; [exact Heq|
    apply EmbedRow_attr_inS_generic, lineitem_attr_inS_tax|apply EmbedRow_dot_tax].
Qed.

Lemma q1_dot_qty_exact_teq :
  forall x t, Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    vexact (Env.env_t TNull nil t)
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.qty x) 2))).
Proof.
  intros x t Heq; apply tnull_dot_decimal_exact.
  eapply q1_teq_interp_dot; [exact Heq|
    apply EmbedRow_attr_inS_generic, lineitem_attr_inS_qty|apply EmbedRow_dot_qty].
Qed.

Lemma q1_inner_select_exact_teq :
  forall x t,
    RowOk_Q1 x ->
    Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil t) (map fst q1_inner_select) (q1_inner_values x).
Proof.
  intros x t Hrow Heq.
  destruct (q1_row_disc_tax_factor_fits x Hrow) as [Hsub Hadd].
  destruct (q1_row_f4_f5_fits x Hrow) as [Hf4 Hf5].
  rewrite q1_inner_select_is_disc_price_charge_shape.
  unfold q1_inner_values, q1_f4_num, q1_f5_num.
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull (EmbedDB_Q1 db))
    (@_instance TNull (EmbedDB_Q1 db))
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched)).
  - apply tnull_dot_string_exact.
    eapply q1_teq_interp_dot; [exact Heq|
      apply EmbedRow_attr_inS_generic, lineitem_attr_inS_rf|apply EmbedRow_dot_rf].
  - apply tnull_dot_string_exact.
    eapply q1_teq_interp_dot; [exact Heq|
      apply EmbedRow_attr_inS_generic, lineitem_attr_inS_ls|apply EmbedRow_dot_ls].
  - apply q1_dot_qty_exact_teq; exact Heq.
  - apply q1_dot_price_exact_teq; exact Heq.
  - eapply tnull_disc_price_exact.
    + apply q1_dot_price_exact_teq; exact Heq.
    + apply q1_dot_disc_exact_teq; exact Heq.
    + exact Hsub.
    + exact Hf4.
  - eapply tnull_charge_exact.
    + apply q1_dot_price_exact_teq; exact Heq.
    + apply q1_dot_disc_exact_teq; exact Heq.
    + apply q1_dot_tax_exact_teq; exact Heq.
    + exact Hsub.
    + exact Hadd.
    + exact Hf4.
    + exact Hf5.
  - apply q1_dot_disc_exact_teq; exact Heq.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched).
Qed.

End Q1Leaves.

Lemma q1_inner_values_embed_teq :
  forall x t,
    Oeset.compare (OTuple TNull) (EmbedRow_Q1 x) t = Eq ->
    q1_inner_values_embed t = q1_inner_values x.
Proof.
  intros x t Heq.
  rewrite <- (q1_inner_values_embed_of_row x).
  unfold q1_inner_values_embed.
  rewrite (pod_teq_dot _ _ _ Heq (EmbedRow_attr_inS_generic x _ lineitem_attr_inS_price)),
    (pod_teq_dot _ _ _ Heq (EmbedRow_attr_inS_generic x _ lineitem_attr_inS_disc)),
    (pod_teq_dot _ _ _ Heq (EmbedRow_attr_inS_generic x _ lineitem_attr_inS_tax)),
    (pod_teq_dot _ _ _ Heq (EmbedRow_attr_inS_generic x _ lineitem_attr_inS_rf)),
    (pod_teq_dot _ _ _ Heq (EmbedRow_attr_inS_generic x _ lineitem_attr_inS_ls)),
    (pod_teq_dot _ _ _ Heq (EmbedRow_attr_inS_generic x _ lineitem_attr_inS_qty)).
  reflexivity.
Qed.

Section Q1Outcomes.
Variable db : list Q1AuxSpec.Row.
Variable delta : Z.
Hypothesis Hpre : Preconditions_Q1 db delta.

Local Abbreviation bs := (@_basesort TNull (EmbedDB_Q1 db)).
Local Abbreviation ins := (@_instance TNull (EmbedDB_Q1 db)).
Local Abbreviation OB := (@outcomes_bag relname bs ins value_is_null).
Local Abbreviation OP := (@outcomes_perm relname bs ins value_is_null).

Local Abbreviation QT := (QExpr_Table q1_table_outputs q1_table).
Local Abbreviation QF := (QExpr_Filter (q1_filter_at delta) QT).
Local Abbreviation QP := (QExpr_Project q1_inner_select QF).

Lemma q1_ao_filter :
  OB QF (map EmbedRow_Q1 (q1_filtered_aux db delta)).
Proof.
  unfold q1_filtered_aux; rewrite <- Q1_filter_map_embed_commute.
  apply outcomes_bag_filter;
    [apply outcomes_bag_table; apply embed_db_table_same_rows|].
  intros sched r r' Hr Heq.
  apply in_map_iff in Hr as [x [<- Hx]].
  assert (Hk : q1_filter_keep delta r' = q1_filter_keep delta (EmbedRow_Q1 x)).
  { unfold q1_filter_keep; rewrite (pod_teq_dot _ _ _ Heq (EmbedRow_shipdate_inS x));
      reflexivity. }
  split; [|exact Hk].
  rewrite Hk.
  pose proof Hpre as [_ [Hdelta [Hrows _]]].
  apply Forall_forall with (x := x) in Hrows; [|exact Hx].
  destruct Hrows as [Hship _].
  rewrite Q1_filter_keep_embed_leb.
  unfold EmbedParams_Q1 in Hdelta |- *.
  rewrite q1_filter_at_is_date_lte_timestamp_add_pred.
  eapply date_lte_timestamp_add_pred_acceptance.
  - exact (pod_teq_inS _ _ _ Heq (EmbedRow_shipdate_inS x)).
  - rewrite (pod_teq_dot _ _ _ Heq (EmbedRow_shipdate_inS x)); apply EmbedRow_dot_shipdate.
  - apply ShipdateOk_finite_date_timestamp_ok; exact Hship.
  - exact q1_base_date_finite_ok.
  - apply ShipdateOk_finite_date_timestamp_ok; exact Hdelta.
Qed.

Lemma q1_ao_inner :
  OP QP (q1_inner_proj_of db delta).
Proof.
  assert (Hmap : q1_inner_proj_of db delta =
    map (fun row => project_row q1_inner_select (q1_inner_values_embed row))
      (map EmbedRow_Q1 (q1_filtered_aux db delta))).
  { unfold q1_inner_proj_of; rewrite map_map; apply map_ext; intro r.
    rewrite q1_inner_values_embed_of_row; reflexivity. }
  rewrite Hmap.
  apply outcomes_bag_project; [exact q1_ao_filter|].
  intros sched r r' Hr Heq.
  apply in_map_iff in Hr as [x [<- Hx]].
  rewrite (q1_inner_values_embed_teq x r' Heq), (q1_inner_values_embed_of_row x).
  split; [|reflexivity].
  apply q1_inner_select_exact_teq; [|exact Heq].
  eapply q1_filtered_aux_RowOk; eassumption.
Qed.

End Q1Outcomes.

(** * Part 2: per-bucket overflow safety for any sub-multiset of the inner rows

    Same proofs as [q1_group_*_safe_of_pre] (Q1FormalSQLSpine), restated over
    [q1_sub_group] instead of membership in the canonical partition, so they
    apply to the groups of every bag representative. *)

Definition q1_sub_group (db : list Q1AuxSpec.Row) (delta : Z)
    (group : list (tuple TNull)) : Prop :=
  (forall row, In row group -> In row (q1_inner_proj_of db delta)) /\
  (List.length group <= List.length db)%nat.

Lemma q1_sub_member_aux_row :
  forall db delta group row,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    In row group ->
    exists r, RowOk_Q1 r /\ row = project_row q1_inner_select (q1_inner_values r).
Proof.
  intros db delta group row Hpre [Hsub _] Hrow.
  destruct (q1_inner_proj_in_inv db delta row (Hsub row Hrow)) as [r [Hinr Heq]].
  exists r; split; [|exact Heq].
  eapply q1_filtered_aux_RowOk; eassumption.
Qed.

Lemma q1_sub_length_le_db :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    (List.length group <= List.length db)%nat.
Proof. intros db delta group _ [_ H]; exact H. Qed.

Lemma q1_group_bucket_count_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.int64_result_runtime_error (Z.of_nat (List.length group)) = None.
Proof.
  intros db delta group Hpre Hsub.
  pose proof (q1_sub_length_le_db db delta group Hpre Hsub) as Hlen.
  destruct Hpre as [_ [_ [_ Hcard]]].
  apply int64_result_runtime_error_none_of_bounds.
  split; [unfold int64_min; lia|lia].
Qed.
Lemma Forall_map_conforms_qty_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    Forall
      (value_conforms_attribute (AttrDecimal "l_quantity" 15 2))
      (map (fun row => dot TNull row q1_attr_qty) group).
Proof.
  intros db delta group Hpre Hin.
  apply Forall_forall; intros v Hv.
  apply in_map_iff in Hv.
  destruct Hv as [row [Heqv Hinrow]].
  subst v.
  destruct (q1_sub_member_aux_row db delta group row Hpre Hin Hinrow)
    as [r [Hok Heq]].
  rewrite Heq, q1_inner_project_dot_qty.
  apply RowOk_qty_conforms; exact Hok.
Qed.

Lemma Forall_map_conforms_price_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    Forall
      (value_conforms_attribute (AttrDecimal "l_extendedprice" 15 2))
      (map (fun row => dot TNull row q1_attr_price) group).
Proof.
  intros db delta group Hpre Hin.
  apply Forall_forall; intros v Hv.
  apply in_map_iff in Hv.
  destruct Hv as [row [Heqv Hinrow]].
  subst v.
  destruct (q1_sub_member_aux_row db delta group row Hpre Hin Hinrow)
    as [r [Hok Heq]].
  rewrite Heq, q1_inner_project_dot_price.
  apply RowOk_price_conforms; exact Hok.
Qed.

Lemma Forall_map_conforms_disc_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    Forall
      (value_conforms_attribute (AttrDecimal "l_discount" 15 2))
      (map (fun row => dot TNull row q1_attr_disc) group).
Proof.
  intros db delta group Hpre Hin.
  apply Forall_forall; intros v Hv.
  apply in_map_iff in Hv.
  destruct Hv as [row [Heqv Hinrow]].
  subst v.
  destruct (q1_sub_member_aux_row db delta group row Hpre Hin Hinrow)
    as [r [Hok Heq]].
  rewrite Heq, q1_inner_project_dot_disc.
  apply RowOk_disc_conforms; exact Hok.
Qed.

Lemma q1_group_sum_qty_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_qty) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_qty) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply fixed_decimal_sum_runtime_safe_of_cardinality
    with (name := "l_quantity") (precision := 15) (scale := 2)
         (max_count := max_count)
         (accumulator_bound := q1_decimal15_sum_accumulator max_count).
  - lia.
  - unfold max_count; lia.
  - exact Hlen.
  - unfold q1_decimal15_sum_accumulator; lia.
  - intros c Hc.
    apply (q1_decimal15_sum_accumulator_fits max_count c); [|exact Hc].
    split; [unfold max_count; lia|exact Hcard].
  - eapply Forall_map_conforms_qty_sub; [exact Hpre|exact Hin].
Qed.

Lemma q1_group_sum_price_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_price) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_price) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply fixed_decimal_sum_runtime_safe_of_cardinality
    with (name := "l_extendedprice") (precision := 15) (scale := 2)
         (max_count := max_count)
         (accumulator_bound := q1_decimal15_sum_accumulator max_count).
  - lia.
  - unfold max_count; lia.
  - exact Hlen.
  - unfold q1_decimal15_sum_accumulator; lia.
  - intros c Hc.
    apply (q1_decimal15_sum_accumulator_fits max_count c); [|exact Hc].
    split; [unfold max_count; lia|exact Hcard].
  - eapply Forall_map_conforms_price_sub; [exact Hpre|exact Hin].
Qed.

(** $f4/$f5 are AttrNumeric carriers equal to [numeric_of_scaled] at scales 4/6. *)
Lemma q1_group_sum_f4_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_f4) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_f4) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply scaled_numeric_sum_runtime_safe_of_cardinality
    with (precision := 15) (scale := 4) (max_count := max_count)
         (accumulator_bound := q1_decimal15_sum_accumulator max_count).
  - lia.
  - unfold max_count; lia.
  - exact Hlen.
  - unfold q1_decimal15_sum_accumulator; lia.
  - intros c Hc.
    apply (q1_scaled_sum_accumulator_fits 4 max_count c);
      [unfold postgres_numeric_max_fractional_digits; lia|
       split; [unfold max_count; lia|exact Hcard]|exact Hc].
  - apply Forall_forall; intros v Hv.
    apply in_map_iff in Hv.
    destruct Hv as [row [Heqv Hinrow]].
    subst v.
    destruct (q1_sub_member_aux_row db delta group row Hpre Hin Hinrow)
      as [r [Hok Heq]].
    rewrite Heq, q1_inner_project_dot_f4, q1_f4_num_of_scaled.
    apply fixed_decimal_value_shape_of_scaled; [lia|].
    apply q1_f4_coeff_bound; exact Hok.
Qed.

Lemma q1_group_sum_f5_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_f5) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  set (acc := max_count * Z.pow 10 17).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_f5) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply scaled_numeric_sum_runtime_safe_of_cardinality
    with (precision := 17) (scale := 6) (max_count := max_count)
         (accumulator_bound := acc).
  - lia.
  - unfold max_count; lia.
  - exact Hlen.
  - unfold acc; lia.
  - intros c Hc.
    apply numeric_of_scaled_runtime_fits_of_digit_bound.
    + unfold postgres_numeric_max_fractional_digits; lia.
    + assert (Habs' : Z.abs c <= int64_max * Z.pow 10 17).
      { eapply Z.le_trans; [exact Hc|].
        unfold acc, max_count.
        apply Z.mul_le_mono_nonneg_r; [apply Z.pow_nonneg; lia|exact Hcard]. }
      assert (Hle : Z.abs c <= Z.pow 10 37).
      { eapply Z.le_trans; [exact Habs'|].
        change (Z.pow 10 17) with 100000000000000000.
        change (Z.pow 10 37) with
          10000000000000000000000000000000000000.
        unfold int64_max; lia. }
      pose proof (numeric_digit_count_le_of_le_pow c 37 ltac:(lia) Hle) as Hd.
      eapply Z.le_trans; [exact Hd|].
      unfold postgres_numeric_max_integer_digits; lia.
  - apply Forall_forall; intros v Hv.
    apply in_map_iff in Hv.
    destruct Hv as [row [Heqv Hinrow]].
    subst v.
    destruct (q1_sub_member_aux_row db delta group row Hpre Hin Hinrow)
      as [r [Hok Heq]].
    rewrite Heq, q1_inner_project_dot_f5, q1_f5_num_of_scaled.
    apply fixed_decimal_value_shape_of_scaled; [lia|].
    apply q1_f5_coeff_bound; exact Hok.
Qed.

Lemma q1_group_avg_qty_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.avg_numeric_fixed_runtime_error 15 2
      (map (fun row => dot TNull row q1_attr_qty) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_qty) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply fixed_decimal_avg_runtime_safe_of_cardinality
    with (name := "l_quantity") (precision := 15) (scale := 2)
         (max_count := max_count).
  - apply numeric_typmod_valid_15_2.
  - lia.
  - exact Hlen.
  - intros fc c Hfc Habs.
    assert (Hfc' : 1 <= fc <= int64_max).
    { destruct Hfc as [Hfc0 Hfc1]. split; [exact Hfc0|].
      eapply Z.le_trans; [exact Hfc1|exact Hcard]. }
    apply decimal15_2_avg_div_runtime_safe; [exact Hfc'|exact Habs].
  - eapply Forall_map_conforms_qty_sub; [exact Hpre|exact Hin].
Qed.

Lemma q1_group_avg_price_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.avg_numeric_fixed_runtime_error 15 2
      (map (fun row => dot TNull row q1_attr_price) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_price) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply fixed_decimal_avg_runtime_safe_of_cardinality
    with (name := "l_extendedprice") (precision := 15) (scale := 2)
         (max_count := max_count).
  - apply numeric_typmod_valid_15_2.
  - lia.
  - exact Hlen.
  - intros fc c Hfc Habs.
    assert (Hfc' : 1 <= fc <= int64_max).
    { destruct Hfc as [Hfc0 Hfc1]. split; [exact Hfc0|].
      eapply Z.le_trans; [exact Hfc1|exact Hcard]. }
    apply decimal15_2_avg_div_runtime_safe; [exact Hfc'|exact Habs].
  - eapply Forall_map_conforms_price_sub; [exact Hpre|exact Hin].
Qed.

Lemma q1_group_avg_disc_safe_of_pre_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    NullValues.avg_numeric_fixed_runtime_error 15 2
      (map (fun row => dot TNull row q1_attr_disc) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_disc) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_sub_length_le_db; [exact Hpre|exact Hin]. }
  assert (Hcard : max_count <= int64_max).
  { unfold max_count. destruct Hpre as [_ [_ [_ Hc]]]; exact Hc. }
  eapply fixed_decimal_avg_runtime_safe_of_cardinality
    with (name := "l_discount") (precision := 15) (scale := 2)
         (max_count := max_count).
  - apply numeric_typmod_valid_15_2.
  - lia.
  - exact Hlen.
  - intros fc c Hfc Habs.
    assert (Hfc' : 1 <= fc <= int64_max).
    { destruct Hfc as [Hfc0 Hfc1]. split; [exact Hfc0|].
      eapply Z.le_trans; [exact Hfc1|exact Hcard]. }
    apply decimal15_2_avg_div_runtime_safe; [exact Hfc'|exact Habs].
  - eapply Forall_map_conforms_disc_sub; [exact Hpre|exact Hin].
Qed.

(** Full per-bucket overflow packaging from Pre (COUNT + SUM×4 + AVG×3). *)
Lemma q1_group_bucket_safe_of_sub :
  forall db delta group,
    Preconditions_Q1 db delta ->
    q1_sub_group db delta group ->
    q1_group_bucket_safe group.
Proof.
  intros db delta group Hpre Hin.
  refine (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ _))))))).
  - eapply q1_group_sum_qty_safe_of_pre_sub; eassumption.
  - eapply q1_group_sum_price_safe_of_pre_sub; eassumption.
  - eapply q1_group_sum_f4_safe_of_pre_sub; eassumption.
  - eapply q1_group_sum_f5_safe_of_pre_sub; eassumption.
  - eapply q1_group_avg_qty_safe_of_pre_sub; eassumption.
  - eapply q1_group_avg_price_safe_of_pre_sub; eassumption.
  - eapply q1_group_avg_disc_safe_of_pre_sub; eassumption.
  - eapply q1_group_bucket_count_safe_of_pre_sub; eassumption.
Qed.

(** * Part 3: Group stage — every outcome is the canonical group bag *)

Local Notation teq := (fun a b : tuple TNull => Oeset.compare (OTuple TNull) a b = Eq).

Lemma q1_map_dot_Forall2 :
  forall a C O, Forall2 teq C O ->
    Forall (fun row => a inS labels TNull row) C ->
    map (fun row => dot TNull row a) O = map (fun row => dot TNull row a) C.
Proof.
  intros a C O HF; induction HF as [|c o C O Hco HF IH]; intros HL; [reflexivity|].
  inversion HL as [|? ? Hc HL']; subst; cbn [map].
  rewrite (pod_teq_dot c o a Hco Hc), (IH HL'); reflexivity.
Qed.

Lemma q1_labels_ok_teq :
  forall c o, teq c o -> q1_group_row_labels_ok c -> q1_group_row_labels_ok o.
Proof.
  intros c o H [H1 [H2 [H3 [H4 [H5 [H6 H7]]]]]].
  cbv beta in H; repeat split; eapply pod_teq_inS; eassumption.
Qed.

Lemma q1_labels_ok_Forall2 :
  forall C O, Forall2 teq C O -> Forall q1_group_row_labels_ok C ->
    Forall q1_group_row_labels_ok O.
Proof.
  intros C O HF; induction HF as [|c o C O Hco HF IH]; intros HL; [constructor|].
  inversion HL; subst; constructor; [eapply q1_labels_ok_teq; eassumption|auto].
Qed.

Ltac q1_attr_forall HL :=
  eapply q1_group_row_labels_Forall_attr; [exact HL|]; intros row Hrow; apply Hrow.

Lemma q1_bucket_safe_Forall2 :
  forall C O, Forall2 teq C O -> Forall q1_group_row_labels_ok C ->
    q1_group_bucket_safe C -> q1_group_bucket_safe O.
Proof.
  intros C O HF HL Hs.
  unfold q1_group_bucket_safe in *.
  rewrite (q1_map_dot_Forall2 q1_attr_qty C O HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_price C O HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_f4 C O HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_f5 C O HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_disc C O HF) by q1_attr_forall HL.
  rewrite <- (Forall2_length HF).
  exact Hs.
Qed.

Lemma q1_comparelA_cons_eq :
  forall (X : Type) (cmp : X -> X -> comparison) a l b m,
    comparelA cmp (a :: l) (b :: m) = Eq -> cmp a b = Eq.
Proof. intros X cmp a l b m H; simpl in H; destruct (cmp a b); congruence. Qed.

(** Group-environment dots read the quicksort head, which is the same up to
    tuple equivalence for any two representatives of the group. *)
Lemma q1_group_env_dot_respect :
  forall g g' a, Oeset.compare (OLTuple TNull) g g' = Eq ->
    Interp.interp_dot TNull (q1_group_env g') a =
    Interp.interp_dot TNull (q1_group_env g) a.
Proof.
  intros g g' a H.
  change (Oeset.compare (mk_oelists (OTuple TNull))
            (ListSort.quicksort (OTuple TNull) g)
            (ListSort.quicksort (OTuple TNull) g') = Eq) in H.
  unfold q1_group_env, Env.env_g.
  rewrite !(Interp.interp_dot_unfold TNull (_ :: nil) a).
  destruct (ListSort.quicksort (OTuple TNull) g) as [|t l];
  destruct (ListSort.quicksort (OTuple TNull) g') as [|t' l'];
    try discriminate H; [reflexivity|].
  pose proof (q1_comparelA_cons_eq _ (Oeset.compare (OTuple TNull)) t l t' l' H) as Ht.
  rewrite <- (Fset.mem_eq_2 _ _ _ (tuple_eq_labels TNull t t' Ht)).
  destruct (a inS? labels TNull t) eqn:Hin; [|reflexivity].
  apply (pod_teq_dot _ _ _ Ht); exact Hin.
Qed.

Lemma q1_fold_add_perm :
  forall l1 l2 : list Z, Permutation l1 l2 ->
    fold_right Z.add 0 l1 = fold_right Z.add 0 l2.
Proof.
  intros l1 l2 H; induction H; simpl; lia.
Qed.

Lemma q1_group_as_aux :
  forall db delta g,
    (forall row, In row g -> In row (q1_inner_proj_of db delta)) ->
    exists rs, g = q1_inner_of_aux rs.
Proof.
  intros db delta g; induction g as [|row g IH]; intros Hsub.
  - exists nil; reflexivity.
  - destruct (q1_inner_proj_in_inv db delta row (Hsub row (or_introl eq_refl)))
      as [r [_ ->]].
    destruct IH as [rs ->]; [intros; apply Hsub; right; assumption|].
    exists (r :: rs); reflexivity.
Qed.

Section Q1GroupOutcomes.
Variable db : list Q1AuxSpec.Row.
Variable delta : Z.
Hypothesis Hpre : Preconditions_Q1 db delta.
Hypothesis Hnum : NumericEmbedFaithful.

Local Abbreviation bs := (@_basesort TNull (EmbedDB_Q1 db)).
Local Abbreviation ins := (@_instance TNull (EmbedDB_Q1 db)).
Local Abbreviation OB := (@outcomes_bag relname bs ins value_is_null).

Local Abbreviation QG :=
  (QExpr_Group q1_group_select q1_group_keys (@SExpr_True TNull relname)
     (QExpr_Project q1_inner_select
        (QExpr_Filter (q1_filter_at delta)
           (QExpr_Table q1_table_outputs q1_table)))).

Local Abbreviation G := (q1_inner_proj_of db delta).

(** A group of any representative factors through a canonical group. *)
Lemma q1_rep_group_factor :
  forall g g',
    In g (query_make_groups nil G q1_group_terms) ->
    Oeset.compare (OLTuple TNull) g g' = Eq ->
    exists g'', Permutation g g'' /\ Forall2 teq g'' g' /\
                q1_sub_group db delta g'' /\
                Forall q1_group_row_labels_ok g''.
Proof.
  intros g g' Hg Hgg.
  destruct (pod_permut_Forall2 _ _ _ _ (pod_OLTuple_permut g g' Hgg)) as [g'' [HP HF]].
  exists g''; split; [exact HP|split; [exact HF|split]].
  - split.
    + intros row Hrow.
      eapply query_make_groups_member_in; [exact Hg|].
      exact (Permutation_in row (Permutation_sym HP) Hrow).
    + rewrite <- (Permutation_length HP).
      eapply q1_group_length_le_db; eassumption.
  - apply Forall_forall; intros row Hrow.
    pose proof (q1_group_buckets_labels_ok db delta g Hg) as HL.
    rewrite Forall_forall in HL; apply HL.
    exact (Permutation_in row (Permutation_sym HP) Hrow).
Qed.

Lemma q1_group_values_respect :
  forall g g',
    In g (query_make_groups nil G q1_group_terms) ->
    Oeset.compare (OLTuple TNull) g g' = Eq ->
    q1_group_select_values g' = q1_group_select_values g.
Proof.
  intros g g' Hg Hgg.
  destruct (q1_rep_group_factor g g' Hg Hgg) as [g'' [HP [HF [Hsub HL]]]].
  destruct (q1_group_as_aux db delta g
              (fun row Hrow => query_make_groups_member_in _ _ _ _ _ Hg Hrow)) as [rs Hrs].
  subst g.
  unfold q1_inner_of_aux in HP.
  destruct (Permutation_map_inv _ _ (Permutation_sym HP)) as [rs'' [Hg'' Hrs]].
  subst g''.
  change (map (fun r => project_row q1_inner_select (q1_inner_values r)) rs'')
    with (q1_inner_of_aux rs'') in *.
  unfold q1_group_select_values.
  rewrite (q1_group_env_dot_respect _ _ q1_attr_rf Hgg),
          (q1_group_env_dot_respect _ _ q1_attr_ls Hgg).
  rewrite (q1_map_dot_Forall2 q1_attr_qty _ _ HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_price _ _ HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_f4 _ _ HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_f5 _ _ HF) by q1_attr_forall HL.
  rewrite (q1_map_dot_Forall2 q1_attr_disc _ _ HF) by q1_attr_forall HL.
  rewrite <- (Forall2_length HF).
  rewrite !q1_inner_dots_qty_map, !q1_inner_dots_price_map, !q1_inner_dots_f4_map,
    !q1_inner_dots_f5_map, !q1_inner_dots_disc_map.
  unfold q1_inner_of_aux; rewrite !length_map.
  destruct (Hnum rs) as [A1 [A2 [A3 [A4 [A5 [A6 A7]]]]]].
  destruct (Hnum rs'') as [B1 [B2 [B3 [B4 [B5 [B6 B7]]]]]].
  rewrite A1, A2, A3, A4, A5, A6, A7, B1, B2, B3, B4, B5, B6, B7.
  rewrite <- (Permutation_length Hrs).
  rewrite (q1_fold_add_perm _ _ (Permutation_map Q1AuxSpec.qty Hrs)),
    (q1_fold_add_perm _ _ (Permutation_map Q1AuxSpec.price Hrs)),
    (q1_fold_add_perm _ _ (Permutation_map q1_row_disc_price Hrs)),
    (q1_fold_add_perm _ _ (Permutation_map q1_row_charge Hrs)),
    (q1_fold_add_perm _ _ (Permutation_map Q1AuxSpec.disc Hrs)).
  reflexivity.
Qed.

Lemma q1_ao_group :
  OB QG (q1_group_rows_of db delta).
Proof.
  unfold q1_group_rows_of.
  apply outcomes_bag_group.
  - exact q1_group_keys_terms.
  - exact q1_group_terms_nonempty.
  - exact (q1_ao_inner db delta Hpre).
  - apply q1_group_dot_keys_runtime_none.
  - intros sched rep Hrep g' Hg'.
    destruct (pod_group_of_rep _ _ _ q1_group_terms_nonempty Hrep g' Hg') as [g [Hg Hgg]].
    destruct (q1_rep_group_factor g g' Hg Hgg) as [g'' [_ [HF [Hsub HL]]]].
    pose proof (q1_group_filter_map_exact db sched g') as Hex.
    rewrite q1_having_is_true in Hex.
    apply Hex.
    + eapply query_make_groups_member_nonempty; [exact q1_group_terms_nonempty|exact Hg'].
    + exact (q1_labels_ok_Forall2 _ _ HF HL).
    + apply (q1_bucket_safe_Forall2 g'' g' HF HL).
      exact (q1_group_bucket_safe_of_sub db delta g'' Hpre Hsub).
  - intros g g' Hg Hgg; exact (q1_group_values_respect g g' Hg Hgg).
Qed.

End Q1GroupOutcomes.

(** * Part 4: ORDER BY + final Project — unique observation *)

Lemma q1_tuple_le_teq :
  forall a a' b b',
    q1_outer_row_labels_ok a -> q1_outer_row_labels_ok b ->
    Oeset.compare (OTuple TNull) a a' = Eq ->
    Oeset.compare (OTuple TNull) b b' = Eq ->
    q1_tuple_le a' b' = q1_tuple_le a b.
Proof.
  intros a a' b b' [Ra [La _]] [Rb [Lb _]] Ha Hb.
  unfold q1_tuple_le, q1_order_keys, SortAscNullsLast, SortKey.
  cbn [compare_order_keys].
  unfold compare_order_key.
  cbn [sort_key_attribute sort_key_value_compare sort_key_direction sort_key_null_direction].
  rewrite (pod_teq_dot a a' _ Ha Ra), (pod_teq_dot a a' _ Ha La),
    (pod_teq_dot b b' _ Hb Rb), (pod_teq_dot b b' _ Hb Lb).
  reflexivity.
Qed.

Lemma q1_outer_labels_teq :
  forall a a', Oeset.compare (OTuple TNull) a a' = Eq ->
    q1_outer_row_labels_ok a -> q1_outer_row_labels_ok a'.
Proof.
  intros a a' H L; unfold q1_outer_row_labels_ok in *.
  repeat match goal with
  | L : _ /\ _ |- _ => destruct L
  end.
  repeat split; eapply pod_teq_inS; eassumption.
Qed.

Lemma q1_outer_dot_values_teq :
  forall a a', Oeset.compare (OTuple TNull) a a' = Eq ->
    q1_outer_row_labels_ok a -> q1_outer_dot_values a' = q1_outer_dot_values a.
Proof.
  intros a a' H [L1 [L2 [L3 [L4 [L5 [L6 [L7 [L8 [L9 L10]]]]]]]]].
  unfold q1_outer_dot_values.
  rewrite (pod_teq_dot a a' _ H L1), (pod_teq_dot a a' _ H L2),
    (pod_teq_dot a a' _ H L3), (pod_teq_dot a a' _ H L4),
    (pod_teq_dot a a' _ H L5), (pod_teq_dot a a' _ H L6),
    (pod_teq_dot a a' _ H L7), (pod_teq_dot a a' _ H L8),
    (pod_teq_dot a a' _ H L9), (pod_teq_dot a a' _ H L10).
  reflexivity.
Qed.

(** Any two possible outcomes of q̂ (under any schedules) coincide. *)
Theorem Q1_qhat_outcomes_unique :
  forall db delta,
    Preconditions_Q1 db delta ->
    NumericEmbedFaithful ->
    forall sched1 o1 sched2 o2,
      @eval_query_expr_outcome TNull relname
        (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched1 nil
        (qhat_Q1 delta) o1 ->
      @eval_query_expr_outcome TNull relname
        (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched2 nil
        (qhat_Q1 delta) o2 ->
      o1 = o2.
Proof.
  intros db delta Hpre Hnum sched1 o1 sched2 o2 Hev1 Hev2.
  set (GO := q1_group_rows_of db delta).
  pose proof (q1_ao_group db delta Hpre Hnum) as HOB; fold GO in HOB.
  assert (HL : forall r, In r GO -> q1_outer_row_labels_ok r)
    by (intros r Hr; exact (q1_group_rows_outer_labels db delta r Hr)).
  assert (Hex : forall sched r r', In r GO ->
            Oeset.compare (OTuple TNull) r r' = Eq ->
            @scalar_value_list_exact_at TNull relname
              (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
              unknown3 NullValues.interp_scalar_operator_runtime_error
              NullValues.interp_aggregate_runtime_error value_is_null sched
              (Env.env_t TNull nil r') (map fst q1_outer_select)
              (q1_outer_dot_values r')).
  { intros sched r r' Hr Heq.
    apply q1_outer_select_exact_of_dots.
    exact (q1_outer_labels_teq r r' Heq (HL r Hr)). }
  assert (Hall : forall sched o,
    @eval_query_expr_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched nil
      (qhat_Q1 delta) o ->
    exists O', Oeset.permut (OTuple TNull) GO O' /\
      @ordered_rows TNull value_is_null q1_order_keys O' /\
      o = SqlSuccess (map (fun r => project_row q1_outer_select (q1_outer_dot_values r)) O')).
  { intros sched o Hev.
    unfold qhat_Q1, query_spine_TFGOP in Hev; rewrite q1_having_is_true in Hev.
    exact (outcomes_order_project _ _ _ q1_outer_select q1_order_keys _ GO
             q1_outer_dot_values HOB Hex sched o Hev). }
  destruct (Hall sched1 o1 Hev1) as [O1 [HP1 [Hord1 ->]]].
  destruct (Hall sched2 o2 Hev2) as [O2 [HP2 [Hord2 ->]]].
  f_equal.
  apply (pod_order_unique _ (fun a b => q1_tuple_le a b = true)
           (fun r => project_row q1_outer_select (q1_outer_dot_values r)) GO).
  - intros a b c Ha Hb Hc Hab Hbc.
    exact (q1_group_rows_le_trans_on db delta Hpre Hnum a b c Ha Hb Hc Hab Hbc).
  - intros a b Ha Hb Hab Hba.
    exact (q1_group_rows_order_keys_antisym db delta a b Hpre Hnum Ha Hb Hab Hba).
  - intros a a' b b' Ha Hb Haa Hbb H.
    rewrite <- (q1_tuple_le_teq a a' b b' (HL a Ha) (HL b Hb) Haa Hbb); exact H.
  - intros a a' Ha Haa; rewrite (q1_outer_dot_values_teq a a' Haa (HL a Ha)); reflexivity.
  - exact HP1.
  - exact HP2.
  - apply Sorted_LocallySorted_iff; exact (ordered_rows_Sorted_Q1 O1 Hord1).
  - apply Sorted_LocallySorted_iff; exact (ordered_rows_Sorted_Q1 O2 Hord2).
Qed.

(** Strengthened NormalizeSound_generic: the embedded accelerator result is
    the ONLY possible FormalSQL outcome of q̂ — every outcome, under every
    Boolean schedule and every bag / ORDER BY representative, equals
    [SqlSuccess outs]. *)
Theorem NormalizeSound_Q1_all_outcomes :
  forall (db : list Q1AuxSpec.Row) (delta : Z),
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    normalize_q1 (qhat_Q1 delta) = Some LogicalSpec_Q1 /\
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db)) /\
    (forall sched o,
       @eval_query_expr_outcome TNull relname
         (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
         unknown3 NullValues.interp_scalar_operator_runtime_error
         NullValues.interp_aggregate_runtime_error value_is_null sched nil
         (qhat_Q1 delta) o ->
       o = SqlSuccess (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db))).
Proof.
  intros db delta Hpre Hsafe Hnum.
  destruct (NormalizeSound_generic db delta Hpre Hsafe Hnum) as [Hn Hx].
  split; [exact Hn|split; [exact Hx|]].
  intros sched o Ho.
  pose proof Hx as Hx'.
  unfold FormalSQL_exec, eval_query_expr_possible_outcome in Hx'.
  destruct Hx' as [sched0 Hx0].
  exact (Q1_qhat_outcomes_unique db delta Hpre Hnum sched o sched0 _ Ho Hx0).
Qed.

(** Catalog instance ([delta = 90], q̂ = the Logos-lowered catalog query). *)
Corollary NormalizeSound_Q1_catalog_all_outcomes :
  forall (db : list Q1AuxSpec.Row),
    Preconditions_Q1 db q1_catalog_delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    FormalSQL_exec (EmbedDB_Q1 db) q1_catalog_delta
      (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 q1_catalog_delta) db)) /\
    (forall sched o,
       @eval_query_expr_outcome TNull relname
         (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
         unknown3 NullValues.interp_scalar_operator_runtime_error
         NullValues.interp_aggregate_runtime_error value_is_null sched nil
         Queries.source_query_expr o ->
       o = SqlSuccess (EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 q1_catalog_delta) db))).
Proof.
  intros db Hpre Hsafe Hnum.
  destruct (NormalizeSound_Q1_all_outcomes db q1_catalog_delta Hpre Hsafe Hnum)
    as [_ [Hx Hall]].
  rewrite qhat_Q1_catalog_instance in Hall.
  split; [exact Hx|exact Hall].
Qed.
