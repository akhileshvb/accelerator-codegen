(* Reusable TNull DATE ≤ (CAST(base AS timestamp) + INTERVAL days) acceptance.

   Packages Logos leaf/call exact + StringTemporalFacts cast/add interp +
   FilterAcceptanceFacade leb bridges into one predicate-acceptance lemma.
   Query proofs supply only [dot] / domain hypotheses — no [EScalar_*]
   nesting in query files. *)

From Stdlib Require Import List String ZArith Lia.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values ValueCore ValueTemporal GenericInstance FTuples
  Interp ListSort.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts StringTemporalFacts
  AggregateRuntimeFacts.
From FormalSQLLib Require Import FilterAcceptanceFacade.

Import Tuple.
Import ListNotations.
Open Scope string_scope.
Open Scope Z_scope.

(** AST shape used by TPC-H Q1-style shipdate cutoffs. *)
Definition date_lte_timestamp_add_pred
    (date_attr_name : string) (base delta : Z)
  : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname
    (PredicateDateLteTimestamp : FTuples.Tuple.predicate TNull)
    [@SExpr_Leaf TNull relname type_date (DotDate date_attr_name)
    ; @SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampDay)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate base)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))]].

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Lemma interp_dot_env_t :
  forall env (row : tuple TNull) attr,
    attr inS labels TNull row ->
    Interp.interp_dot TNull (Env.env_t TNull env row) attr =
    dot TNull row attr.
Proof.
  intros env row attr Hin.
  unfold Env.env_t.
  rewrite (Interp.interp_dot_unfold TNull (_ :: env) attr).
  rewrite (quicksort_1 (OTuple TNull) row).
  rewrite Hin.
  reflexivity.
Qed.

Lemma finite_date_cast_checked :
  forall d,
    finite_date_timestamp_ok d ->
    cast_date_to_timestamp_checked d = Some (cast_date_to_timestamp d).
Proof.
  intros d [_ [Hts [Hninf Hpinf]]].
  apply cast_date_to_timestamp_checked_finite; try assumption.
  apply timestamp_in_range_bool_true_iff; exact Hts.
Qed.

Lemma tnull_first_observation_error_none :
  forall values,
    NullValues.first_observation_error
      (map (fun v : NullValues.value =>
        (@None sql_runtime_error, v)) values) = None.
Proof.
  induction values as [|v vs IH]; [reflexivity|].
  cbn; exact IH.
Qed.

Lemma tnull_observation_values_none_map :
  forall values,
    NullValues.observation_values
      (map (fun v : NullValues.value =>
        (@None sql_runtime_error, v)) values) = values.
Proof.
  induction values as [|v vs IH]; [reflexivity|].
  cbn; f_equal; exact IH.
Qed.

Lemma tnull_scalar_call_success_of_local :
  forall operator values expected,
    match operator with ScalarCase => False | _ => True end ->
    NullValues.scalar_operator_local_runtime_error operator values = None ->
    NullValues.interp_scalar_operator operator values = expected ->
    @scalar_call_value_outcome TNull
      NullValues.interp_scalar_operator_runtime_error
      operator values = SqlSuccess expected.
Proof.
  intros operator values expected.
  unfold scalar_call_value_outcome, NullValues.interp_scalar_operator_runtime_error.
  destruct operator; intros Hok Hlocal Hinterp; try (exfalso; exact Hok);
    rewrite tnull_first_observation_error_none;
    rewrite tnull_observation_values_none_map;
    rewrite Hlocal;
    cbn;
    apply f_equal;
    exact Hinterp.
Qed.

Lemma tnull_leaf_dot_exact :
  forall basesort instance boolean_schedule env result_type attr expected,
    Interp.interp_dot TNull env attr = expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (SExpr_Leaf result_type (AExpr (Dot attr))) expected.
Proof.
  intros basesort instance boolean_schedule env result_type attr expected Hinterp.
  apply (@scalar_value_expr_leaf_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  unfold scalar_leaf_value_outcome.
  simpl.
  rewrite Hinterp; reflexivity.
Qed.

Lemma tnull_leaf_constant_exact :
  forall basesort instance boolean_schedule env result_type c,
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (SExpr_Leaf result_type (AExpr (Constant c))) c.
Proof.
  intros basesort instance boolean_schedule env result_type c.
  apply (@scalar_value_expr_leaf_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  unfold scalar_leaf_value_outcome.
  simpl; reflexivity.
Qed.

Lemma tnull_cast_date_to_timestamp_exact :
  forall basesort instance boolean_schedule env date,
    finite_date_timestamp_ok date ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_timestamp
        (ScalarCast ScalarCastDateToTimestamp)
        [@SExpr_Leaf TNull relname type_date (CstDate date)])
      (NullValues.Value_timestamp (Some (cast_date_to_timestamp date))).
Proof.
  intros basesort instance boolean_schedule env date Hok.
  pose proof (finite_date_cast_checked date Hok) as Hcast.
  pose proof (scalar_cast_date_to_timestamp_success_safe date
    (cast_date_to_timestamp date) Hcast) as [Hinterp Hlocal].
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
    + change (CstDate date) with
        (AExpr (Constant (NullValues.Value_date (Some date)))).
      apply tnull_leaf_constant_exact.
    + apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local;
      [exact I|exact Hlocal|exact Hinterp].
Qed.

Lemma tnull_timestamp_add_days_exact :
  forall basesort instance boolean_schedule env base delta,
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok (base - delta) ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampDay)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate base)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))])
      (NullValues.Value_timestamp
        (Some (cast_date_to_timestamp (base - delta)))).
Proof.
  intros basesort instance boolean_schedule env base delta Hbase Hcut.
  pose proof (timestamp_add_neg_days_cast_cutoff base delta Hbase Hcut)
    as Hadd.
  pose proof (timestamp_scalar_add_checked_success ScalarTimestampDay
    (cast_date_to_timestamp base) (-delta)
    (cast_date_to_timestamp (base - delta)) Hadd) as [Hinterp Hlocal].
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
    + apply tnull_cast_date_to_timestamp_exact; exact Hbase.
    + eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
      * change (CstZ (-delta)) with
          (AExpr (Constant (NullValues.Value_Z (Some (-delta))))).
        apply tnull_leaf_constant_exact.
      * apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error
          tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local;
      [exact I|exact Hlocal|exact Hinterp].
Qed.

(** Main facade: predicate acceptance equals [Z.leb d (base - delta)]. *)
Theorem date_lte_timestamp_add_pred_acceptance :
  forall basesort instance boolean_schedule outer row
      date_attr_name base delta d,
    AttrDate date_attr_name inS labels TNull row ->
    dot TNull row (AttrDate date_attr_name) =
      NullValues.Value_date (Some d) ->
    finite_date_timestamp_ok d ->
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok (base - delta) ->
    @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_t TNull outer row)
      (date_lte_timestamp_add_pred date_attr_name base delta)
      (Z.leb d (base - delta)).
Proof.
  intros basesort instance boolean_schedule outer row
    date_attr_name base delta d Hin Hdot Hd Hbase Hcut.
  set (env := Env.env_t TNull outer row).
  assert (Hdot_env :
    Interp.interp_dot TNull env (AttrDate date_attr_name) =
    NullValues.Value_date (Some d)).
  { unfold env; rewrite interp_dot_env_t by exact Hin; exact Hdot. }
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      [@SExpr_Leaf TNull relname type_date (DotDate date_attr_name)
      ; @SExpr_Call TNull relname type_timestamp
          (ScalarTimestampAdd ScalarTimestampDay)
          [@SExpr_Call TNull relname type_timestamp
             (ScalarCast ScalarCastDateToTimestamp)
             [@SExpr_Leaf TNull relname type_date (CstDate base)]
          ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))]]
      [NullValues.Value_date (Some d);
       NullValues.Value_timestamp
         (Some (cast_date_to_timestamp (base - delta)))]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname basesort instance
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error tnull_value_is_null
      boolean_schedule env
      (@SExpr_Leaf TNull relname type_date (DotDate date_attr_name))
      [@SExpr_Call TNull relname type_timestamp
          (ScalarTimestampAdd ScalarTimestampDay)
          [@SExpr_Call TNull relname type_timestamp
             (ScalarCast ScalarCastDateToTimestamp)
             [@SExpr_Leaf TNull relname type_date (CstDate base)]
          ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))]]
      (NullValues.Value_date (Some d))
      [NullValues.Value_timestamp
         (Some (cast_date_to_timestamp (base - delta)))]).
    - unfold DotDate.
      apply tnull_leaf_dot_exact; exact Hdot_env.
    - eapply (@scalar_value_list_exact_cons TNull relname basesort instance
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error tnull_value_is_null
        boolean_schedule env
        (@SExpr_Call TNull relname type_timestamp
          (ScalarTimestampAdd ScalarTimestampDay)
          [@SExpr_Call TNull relname type_timestamp
             (ScalarCast ScalarCastDateToTimestamp)
             [@SExpr_Leaf TNull relname type_date (CstDate base)]
          ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))])
        (@nil (scalar_expr TNull relname ScalarResultValue))
        (NullValues.Value_timestamp
           (Some (cast_date_to_timestamp (base - delta))))
        (@nil NullValues.value)).
      + apply (tnull_timestamp_add_days_exact basesort instance
          boolean_schedule env base delta Hbase Hcut).
      + apply (@scalar_value_list_exact_nil TNull relname basesort instance
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error tnull_value_is_null
          boolean_schedule env).
  }
  pose proof
    (@pred_acceptance_of_value_list TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      PredicateDateLteTimestamp
      [@SExpr_Leaf TNull relname type_date (DotDate date_attr_name)
      ; @SExpr_Call TNull relname type_timestamp
          (ScalarTimestampAdd ScalarTimestampDay)
          [@SExpr_Call TNull relname type_timestamp
             (ScalarCast ScalarCastDateToTimestamp)
             [@SExpr_Leaf TNull relname type_date (CstDate base)]
          ; @SExpr_Leaf TNull relname type_Z (CstZ (-delta))]]
      [NullValues.Value_date (Some d);
       NullValues.Value_timestamp
         (Some (cast_date_to_timestamp (base - delta)))]
      Hargs) as Hacc.
  unfold date_lte_timestamp_add_pred.
  replace (Z.leb d (base - delta)) with
    (Bool.is_true (B TNull)
      (interp_predicate TNull PredicateDateLteTimestamp
        [NullValues.Value_date (Some d);
         NullValues.Value_timestamp
           (Some (cast_date_to_timestamp (base - delta)))])).
  - exact Hacc.
  - rewrite <- (date_lte_cast_cutoff_leb d (base - delta) Hd Hcut).
    reflexivity.
Qed.

(** * Year-window upper bound (TPC-H Q12 style)

    [receipt < CAST(base AS timestamp) + INTERVAL '1' YEAR]
    with Spec end date = [date_add_months base 12]. *)

Definition date_lt_timestamp_add_year_pred
    (date_attr_name : string) (base : Z)
  : @scalar_expr TNull relname ScalarResultBoolean :=
  @SExpr_Pred TNull relname
    (PredicateDateLtTimestamp : FTuples.Tuple.predicate TNull)
    [@SExpr_Leaf TNull relname type_date (DotDate date_attr_name)
    ; @SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampYear)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate base)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ 1)]].

Lemma q12_catalog_year_end_date :
  date_add_months 9131 12 = 9496.
Proof. vm_compute. reflexivity. Qed.

Lemma q12_catalog_year_end_timestamp :
  timestamp_add_years (cast_date_to_timestamp 9131) 1 =
  cast_date_to_timestamp 9496.
Proof. vm_compute. reflexivity. Qed.

Lemma tnull_timestamp_add_year_exact :
  forall basesort instance boolean_schedule env base end_date,
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok end_date ->
    timestamp_add_years (cast_date_to_timestamp base) 1 =
      cast_date_to_timestamp end_date ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampYear)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate base)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ 1)])
      (NullValues.Value_timestamp
        (Some (cast_date_to_timestamp end_date))).
Proof.
  intros basesort instance boolean_schedule env base end_date
    Hbase Hend Heq.
  assert (Hchecked :
    timestamp_add_years_checked (cast_date_to_timestamp base) 1 =
    Some (cast_date_to_timestamp end_date)).
  {
    unfold timestamp_add_years_checked.
    destruct Hbase as [_ [Hbase_ts _]].
    destruct Hend as [_ [Hend_ts _]].
    rewrite (timestamp_infinity_false_of_in_range _ Hbase_ts).
    unfold timestamp_checked.
    rewrite Heq.
    apply timestamp_in_range_bool_true_iff in Hend_ts.
    rewrite Hend_ts.
    reflexivity.
  }
  pose proof (timestamp_scalar_add_checked_success ScalarTimestampYear
    (cast_date_to_timestamp base) 1
    (cast_date_to_timestamp end_date) Hchecked) as [Hinterp Hlocal].
  eapply (@scalar_value_expr_call_exact TNull relname basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule).
  - eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule).
    + apply tnull_cast_date_to_timestamp_exact; exact Hbase.
    + eapply (@scalar_value_list_exact_cons TNull relname basesort instance unknown3
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        tnull_value_is_null boolean_schedule).
      * change (CstZ 1) with
          (AExpr (Constant (NullValues.Value_Z (Some 1)))).
        apply tnull_leaf_constant_exact.
      * apply (@scalar_value_list_exact_nil TNull relname basesort instance unknown3
          NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error
          tnull_value_is_null boolean_schedule).
  - apply tnull_scalar_call_success_of_local;
      [exact I|exact Hlocal|exact Hinterp].
Qed.

Lemma tnull_timestamp_add_year_9131_exact :
  forall basesort instance boolean_schedule env,
    finite_date_timestamp_ok 9131 ->
    finite_date_timestamp_ok 9496 ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (@SExpr_Call TNull relname type_timestamp
        (ScalarTimestampAdd ScalarTimestampYear)
        [@SExpr_Call TNull relname type_timestamp
           (ScalarCast ScalarCastDateToTimestamp)
           [@SExpr_Leaf TNull relname type_date (CstDate 9131)]
        ; @SExpr_Leaf TNull relname type_Z (CstZ 1)])
      (NullValues.Value_timestamp
        (Some (cast_date_to_timestamp 9496))).
Proof.
  intros basesort instance boolean_schedule env H9131 H9496.
  apply tnull_timestamp_add_year_exact; [exact H9131|exact H9496|].
  exact q12_catalog_year_end_timestamp.
Qed.

(** [receipt < CAST(base AS timestamp) + 1 YEAR] acceptance = [Z.ltb d end_date]
    when [timestamp_add_years (cast base) 1 = cast end_date]. *)
Theorem date_lt_timestamp_add_year_pred_acceptance :
  forall basesort instance boolean_schedule outer row
      date_attr_name base end_date d,
    AttrDate date_attr_name inS labels TNull row ->
    dot TNull row (AttrDate date_attr_name) =
      NullValues.Value_date (Some d) ->
    finite_date_timestamp_ok d ->
    finite_date_timestamp_ok base ->
    finite_date_timestamp_ok end_date ->
    timestamp_add_years (cast_date_to_timestamp base) 1 =
      cast_date_to_timestamp end_date ->
    @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_t TNull outer row)
      (date_lt_timestamp_add_year_pred date_attr_name base)
      (Z.ltb d end_date).
Proof.
  intros basesort instance boolean_schedule outer row
    date_attr_name base end_date d Hin Hdot Hd Hbase Hend Heq.
  set (env := Env.env_t TNull outer row).
  assert (Hdot_env :
    Interp.interp_dot TNull env (AttrDate date_attr_name) =
    NullValues.Value_date (Some d)).
  { unfold env; rewrite interp_dot_env_t by exact Hin; exact Hdot. }
  assert (Hargs :
    @scalar_value_list_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      [@SExpr_Leaf TNull relname type_date (DotDate date_attr_name)
      ; @SExpr_Call TNull relname type_timestamp
          (ScalarTimestampAdd ScalarTimestampYear)
          [@SExpr_Call TNull relname type_timestamp
             (ScalarCast ScalarCastDateToTimestamp)
             [@SExpr_Leaf TNull relname type_date (CstDate base)]
          ; @SExpr_Leaf TNull relname type_Z (CstZ 1)]]
      [NullValues.Value_date (Some d);
       NullValues.Value_timestamp
         (Some (cast_date_to_timestamp end_date))]).
  {
    eapply (@scalar_value_list_exact_cons TNull relname basesort instance
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error tnull_value_is_null
      boolean_schedule).
    - unfold DotDate.
      apply tnull_leaf_dot_exact; exact Hdot_env.
    - eapply (@scalar_value_list_exact_cons TNull relname basesort instance
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error tnull_value_is_null
        boolean_schedule).
      + apply (tnull_timestamp_add_year_exact basesort instance
          boolean_schedule env base end_date Hbase Hend Heq).
      + apply (@scalar_value_list_exact_nil TNull relname basesort instance
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error tnull_value_is_null
          boolean_schedule).
  }
  pose proof
    (@pred_acceptance_of_value_list TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      PredicateDateLtTimestamp
      [@SExpr_Leaf TNull relname type_date (DotDate date_attr_name)
      ; @SExpr_Call TNull relname type_timestamp
          (ScalarTimestampAdd ScalarTimestampYear)
          [@SExpr_Call TNull relname type_timestamp
             (ScalarCast ScalarCastDateToTimestamp)
             [@SExpr_Leaf TNull relname type_date (CstDate base)]
          ; @SExpr_Leaf TNull relname type_Z (CstZ 1)]]
      [NullValues.Value_date (Some d);
       NullValues.Value_timestamp
         (Some (cast_date_to_timestamp end_date))]
      Hargs) as Hacc.
  unfold date_lt_timestamp_add_year_pred.
  replace (Z.ltb d end_date) with
    (Bool.is_true (B TNull)
      (interp_predicate TNull PredicateDateLtTimestamp
        [NullValues.Value_date (Some d);
         NullValues.Value_timestamp
           (Some (cast_date_to_timestamp end_date))])).
  - exact Hacc.
  - rewrite <- (date_lt_cast_cutoff_ltb d end_date Hd Hend).
    reflexivity.
Qed.

Lemma date_lt_timestamp_add_year_9131_acceptance :
  forall basesort instance boolean_schedule outer row d,
    AttrDate "l_receiptdate" inS labels TNull row ->
    dot TNull row (AttrDate "l_receiptdate") =
      NullValues.Value_date (Some d) ->
    finite_date_timestamp_ok d ->
    finite_date_timestamp_ok 9131 ->
    finite_date_timestamp_ok 9496 ->
    @scalar_expr_acceptance_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_t TNull outer row)
      (date_lt_timestamp_add_year_pred "l_receiptdate" 9131)
      (Z.ltb d 9496).
Proof.
  intros; eapply date_lt_timestamp_add_year_pred_acceptance; try eassumption.
  exact q12_catalog_year_end_timestamp.
Qed.

