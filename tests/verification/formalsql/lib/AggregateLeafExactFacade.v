(* Reusable TNull aggregate-leaf exactness facade.

   Packages Logos [scalar_value_expr_leaf_exact] + AggregateOutcomeBridgeFacts /
   NumericRegroupFacts / AggregateRuntimeFacts value/runtime bridges so query
   proofs obtain [scalar_value_expr_exact_at] for COUNT-star / SUM(dot) /
   AVG(fixed,p,s)(dot) aggregate leaves from interp + runtime-None premises —
   no [EScalar_Leaf] nesting in query files. *)

From Stdlib Require Import List Sorting.Permutation ZArith Lia.
From SQLFS Require Import
  OrderedSet FiniteSet FiniteBag FiniteCollection FlatData Env Bool3
  SqlOutcome SqlErrorSemantics SqlOrder SqlQuerySyntax SqlQuerySemantics
  SqlQueryFacts Values ValueCore ValueNumeric ValueInteger GenericInstance
  FTuples Interp ATerms FTerms.
From Logos.FormalSQL Require Import
  TNullSyntax QueryTNullSyntax GroupedFilterOutcomeFacts
  AggregateOutcomeBridgeFacts NumericRegroupFacts NumericFacts
  AggregateRuntimeFacts.

Import Tuple.
Import ListNotations.
Import NullValues.
Open Scope Z_scope.

Definition tnull_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

(** Leaf exactness from a successful [scalar_leaf_value_outcome]. *)
Theorem tnull_aggregate_leaf_exact :
  forall basesort instance boolean_schedule env result_type term expected,
    @scalar_leaf_value_outcome TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      env term = SqlSuccess expected ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (SExpr_Leaf result_type term) expected.
Proof.
  intros; eapply (@scalar_value_expr_leaf_exact TNull relname
    basesort instance unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    tnull_value_is_null boolean_schedule); eassumption.
Qed.

(** Leaf exactness from matching interp + full runtime-error checker = None. *)
Theorem tnull_aggregate_leaf_exact_of_interp :
  forall basesort instance boolean_schedule env result_type term expected,
    Interp.interp_aggterm TNull env term = expected ->
    @eval_aggterm_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      env term = None ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule env
      (SExpr_Leaf result_type term) expected.
Proof.
  intros basesort instance boolean_schedule env result_type term expected
    Hinterp Hnone.
  apply tnull_aggregate_leaf_exact.
  unfold scalar_leaf_value_outcome.
  rewrite Hnone, Hinterp; reflexivity.
Qed.

(** COUNT-star over a closed group environment. *)
Theorem tnull_count_star_leaf_exact :
  forall basesort instance boolean_schedule env group_terms group,
    int64_result_runtime_error (Z.of_nat (List.length group)) = None ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_g TNull env (@Env.Group_By TNull group_terms) group)
      (@SExpr_Leaf TNull relname type_int64 ACountStar)
      (value_int64_checked (Z.of_nat (List.length group))).
Proof.
  intros basesort instance boolean_schedule env group_terms group Hsafe.
  destruct (tnull_group_count_star_value_runtime_exact
    env group_terms group) as [Hval [_ Hrun]].
  eapply tnull_aggregate_leaf_exact_of_interp.
  - exact Hval.
  - rewrite Hrun; exact Hsafe.
Qed.

(** SUM(dot attr) over a nonempty closed group with labeled rows. *)
Theorem tnull_sum_numeric_dot_leaf_exact :
  forall basesort instance boolean_schedule group_terms group attr,
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row attr) group) = None ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (@SExpr_Leaf TNull relname type_numeric
        (tnull_sum_numeric_dot_term attr))
      (NullValues.interp_sum_numeric
        (map (fun row => dot TNull row attr) group)).
Proof.
  intros basesort instance boolean_schedule group_terms group attr
    Hne Hlab Hsafe.
  destruct (@tnull_closed_group_sum_numeric_dot_value_runtime_exact
    group_terms group attr Hne Hlab) as [Hval Hagg].
  (* For [A_agg], full runtime error coincides with the aggregate-only checker. *)
  assert (Hrun :
    @eval_aggterm_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_numeric_dot_term attr) =
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_numeric_dot_term attr)).
  {
    unfold tnull_sum_numeric_dot_term, AAggregate.
    reflexivity.
  }
  eapply tnull_aggregate_leaf_exact_of_interp.
  - exact Hval.
  - rewrite Hrun, Hagg; exact Hsafe.
Qed.

(** Aggregate-finalize checker for COUNT-star equals the int64 length check. *)
Theorem tnull_count_star_aggregate_runtime_exact :
  forall env group_terms group,
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull env (@Env.Group_By TNull group_terms) group)
      ACountStar =
    int64_result_runtime_error (Z.of_nat (List.length group)).
Proof.
  intros env group_terms group.
  exact (proj1 (proj2 (tnull_group_count_star_value_runtime_exact
    env group_terms group))).
Qed.

(** Aggregate-finalize checker for SUM(dot) equals the numeric sum check. *)
Theorem tnull_sum_numeric_dot_aggregate_runtime_exact :
  forall group_terms group attr,
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_numeric_dot_term attr) =
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row attr) group).
Proof.
  intros group_terms group attr Hne Hlab.
  exact (proj2 (@tnull_closed_group_sum_numeric_dot_value_runtime_exact
    group_terms group attr Hne Hlab)).
Qed.

(** AVG(fixed precision,scale) over a direct column — term shape. *)
Definition tnull_avg_numeric_fixed_dot_term
    (precision scale : Z) (attribute : attribute TNull) :=
  AAggregate (AggregateAverageNumericFixed precision scale)
    AggregateAll (Dot attribute).

Definition tnull_avg_numeric_fixed_dot_call
    (precision scale : Z) : ValueCore.aggregate :=
  AggregateCall (AggregateAverageNumericFixed precision scale) AggregateAll.

(** Closed-group value + aggregate-finalize runtime for AVG(fixed)(dot),
    packaged from [closed_group_direct_column_argument_observations_permutation_rows]
    + NumericFacts AVG permutation lemmas (same shape as SUM's Logos bridge). *)
Theorem tnull_closed_group_avg_numeric_fixed_dot_value_runtime_exact :
  forall precision scale group_terms group attribute,
    group <> nil ->
    Forall
      (fun row => attribute inS labels TNull row)
      group ->
    Interp.interp_aggterm TNull
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attribute) =
      NullValues.interp_avg_numeric_fixed precision scale
        (map (fun row => dot TNull row attribute) group) /\
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attribute) =
      NullValues.avg_numeric_fixed_runtime_error precision scale
        (map (fun row => dot TNull row attribute) group).
Proof.
  intros precision scale group_terms group attribute Hnonempty Hpresent.
  set (call := tnull_avg_numeric_fixed_dot_call precision scale).
  set (obs :=
    @closed_group_direct_column_argument_observations TNull
      NullValues.interp_scalar_operator_runtime_error
      group_terms group call attribute).
  pose proof
    (@closed_group_direct_column_argument_observations_permutation_rows
      TNull NullValues.interp_scalar_operator_runtime_error
      group_terms group call attribute Hnonempty Hpresent)
    as Hobservations.
  assert (Hvalues :
    Permutation
      (NullValues.observation_values obs)
      (map (fun row => dot TNull row attribute) group)).
  {
    unfold NullValues.observation_values.
    eapply Permutation_trans with
      (l' :=
        map snd
          (map
            (fun row => (None, dot TNull row attribute))
            group)).
    - now apply Permutation_map.
    - rewrite map_map; apply Permutation_refl.
  }
  assert (Hchildren :
    NullValues.first_observation_error obs = None).
  {
    apply first_observation_error_none_iff.
    rewrite Forall_forall; intros observation Hobservation.
    assert (Hmapped : In observation
      (map (fun row => (None, dot TNull row attribute)) group)).
    { eapply Permutation_in; eauto. }
    apply in_map_iff in Hmapped.
    destruct Hmapped as [row [Hequal _]].
    now subst observation.
  }
  assert (Hval_obs :
    Interp.interp_aggterm TNull
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attribute) =
    NullValues.interp_avg_numeric_fixed precision scale
      (NullValues.observation_values obs)).
  {
    unfold tnull_avg_numeric_fixed_dot_term, tnull_avg_numeric_fixed_dot_call,
      AAggregate, obs, call, closed_group_direct_column_argument_observations,
      closed_group_direct_column_argument_envs, direct_column_aggregate_term,
      NullValues.observation_values.
    cbn [Interp.interp_aggterm NullValues.interp_aggregate
      NullValues.interp_aggregate_function
      NullValues.aggregate_input_values].
    rewrite map_map; reflexivity.
  }
  assert (Hrun_obs :
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attribute) =
    NullValues.interp_aggregate_runtime_error call obs).
  {
    unfold tnull_avg_numeric_fixed_dot_term, tnull_avg_numeric_fixed_dot_call,
      AAggregate, obs, call, closed_group_direct_column_argument_observations,
      closed_group_direct_column_argument_envs, direct_column_aggregate_term.
    reflexivity.
  }
  split.
  - rewrite Hval_obs.
    exact (@NumericFacts.interp_avg_numeric_fixed_permutation
      precision scale _ _ Hvalues).
  - rewrite Hrun_obs.
    unfold call, tnull_avg_numeric_fixed_dot_call.
    rewrite (@aggregate_call_safe_children_reduce_to_local
      (AggregateAverageNumericFixed precision scale) AggregateAll obs
      Hchildren).
    cbn [NullValues.aggregate_local_runtime_error
      NullValues.aggregate_local_runtime_error_function
      NullValues.aggregate_input_values].
    exact (@NumericFacts.avg_numeric_fixed_runtime_error_permutation
      precision scale _ _ Hvalues).
Qed.

(** AVG(fixed)(dot) leaf exactness over a nonempty closed group. *)
Theorem tnull_avg_numeric_fixed_dot_leaf_exact :
  forall basesort instance boolean_schedule precision scale
      group_terms group attr,
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    NullValues.avg_numeric_fixed_runtime_error precision scale
      (map (fun row => dot TNull row attr) group) = None ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (@SExpr_Leaf TNull relname type_numeric
        (tnull_avg_numeric_fixed_dot_term precision scale attr))
      (NullValues.interp_avg_numeric_fixed precision scale
        (map (fun row => dot TNull row attr) group)).
Proof.
  intros basesort instance boolean_schedule precision scale
    group_terms group attr Hne Hlab Hsafe.
  destruct (@tnull_closed_group_avg_numeric_fixed_dot_value_runtime_exact
    precision scale group_terms group attr Hne Hlab) as [Hval Hagg].
  assert (Hrun :
    @eval_aggterm_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attr) =
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attr)).
  {
    unfold tnull_avg_numeric_fixed_dot_term, AAggregate.
    reflexivity.
  }
  eapply tnull_aggregate_leaf_exact_of_interp.
  - exact Hval.
  - rewrite Hrun, Hagg; exact Hsafe.
Qed.

(** Aggregate-finalize checker for AVG(fixed)(dot) equals the fixed AVG check. *)
Theorem tnull_avg_numeric_fixed_dot_aggregate_runtime_exact :
  forall precision scale group_terms group attr,
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_avg_numeric_fixed_dot_term precision scale attr) =
    NullValues.avg_numeric_fixed_runtime_error precision scale
      (map (fun row => dot TNull row attr) group).
Proof.
  intros precision scale group_terms group attr Hne Hlab.
  exact (proj2 (@tnull_closed_group_avg_numeric_fixed_dot_value_runtime_exact
    precision scale group_terms group attr Hne Hlab)).
Qed.

(** * SUM(Int32)→Int64 term shape (TPC-H Q12 CASE counts)

    Closed-group value/runtime exactness is the Int32 twin of
    [tnull_closed_group_sum_numeric_dot_value_runtime_exact]; Logos already
    lists [AggregateSumInt32] in
    [closed_group_direct_column_exact_sum_outcome_permutation_rows]. *)
Definition tnull_sum_int32_dot_term (attribute : attribute TNull) :=
  AAggregate AggregateSumInt32 AggregateAll (Dot attribute).

Definition tnull_sum_int32_dot_call : ValueCore.aggregate :=
  AggregateCall AggregateSumInt32 AggregateAll.

Lemma tnull_sum_int32_call_in_exact_sum_family :
  In tnull_sum_int32_dot_call
    [Aggregate AggregateSumInt32;
     DistinctAggregate AggregateSumInt32;
     Aggregate AggregateSumInt64Numeric;
     DistinctAggregate AggregateSumInt64Numeric;
     Aggregate AggregateSumNumeric;
     DistinctAggregate AggregateSumNumeric].
Proof. now left. Qed.

Lemma tnull_sum_int32_dot_term_eq_direct :
  forall attribute,
    tnull_sum_int32_dot_term attribute =
    @direct_column_aggregate_term TNull tnull_sum_int32_dot_call attribute.
Proof.
  intro attribute.
  unfold tnull_sum_int32_dot_term, tnull_sum_int32_dot_call,
    direct_column_aggregate_term, AAggregate, Dot.
  reflexivity.
Qed.

(** Argument environments / observations for closed-group SUM(Int32)(dot).
    Definitionally aligned with [interp_aggterm] / aggregate runtime checker. *)
Definition tnull_closed_group_sum_int32_dot_argument_envs
    (group_terms : list (@aggterm TNull))
    (group : list (tuple TNull))
    (attribute : attribute TNull) :=
  @closed_group_direct_column_argument_envs TNull
    group_terms group tnull_sum_int32_dot_call attribute.

Definition tnull_closed_group_sum_int32_dot_argument_observations
    (group_terms : list (@aggterm TNull))
    (group : list (tuple TNull))
    (attribute : attribute TNull) :=
  @closed_group_direct_column_argument_observations TNull
    NullValues.interp_scalar_operator_runtime_error
    group_terms group tnull_sum_int32_dot_call attribute.

Definition tnull_closed_group_sum_int32_dot_argument_values
    (group_terms : list (@aggterm TNull))
    (group : list (tuple TNull))
    (attribute : attribute TNull) :=
  map
    (fun argument_env =>
      Interp.interp_funterm TNull argument_env (Dot attribute))
    (tnull_closed_group_sum_int32_dot_argument_envs
      group_terms group attribute).

Lemma tnull_closed_group_sum_int32_dot_observations_perm :
  forall group_terms group attribute,
    group <> nil ->
    Forall (fun row => attribute inS labels TNull row) group ->
    Permutation
      (tnull_closed_group_sum_int32_dot_argument_observations
        group_terms group attribute)
      (map (fun row => (None, dot TNull row attribute)) group).
Proof.
  intros group_terms group attribute Hne Hlab.
  unfold tnull_closed_group_sum_int32_dot_argument_observations.
  apply (@closed_group_direct_column_argument_observations_permutation_rows
    TNull NullValues.interp_scalar_operator_runtime_error
    group_terms group tnull_sum_int32_dot_call attribute Hne Hlab).
Qed.

Local Lemma tnull_sum_int32_dot_observation_values :
  forall group_terms group attribute,
    NullValues.observation_values
      (tnull_closed_group_sum_int32_dot_argument_observations
        group_terms group attribute) =
    tnull_closed_group_sum_int32_dot_argument_values
      group_terms group attribute.
Proof.
  intros group_terms group attribute.
  unfold NullValues.observation_values,
    tnull_closed_group_sum_int32_dot_argument_observations,
    tnull_closed_group_sum_int32_dot_argument_values,
    closed_group_direct_column_argument_observations,
    tnull_closed_group_sum_int32_dot_argument_envs,
    closed_group_direct_column_argument_envs, Dot.
  now rewrite map_map.
Qed.

Local Lemma tnull_sum_int32_dot_value_observations :
  forall group_terms group attribute,
    Interp.interp_aggterm TNull
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attribute) =
    NullValues.interp_sum_int32_as_int64
      (tnull_closed_group_sum_int32_dot_argument_values
        group_terms group attribute).
Proof.
  intros group_terms group attribute.
  unfold tnull_closed_group_sum_int32_dot_argument_values,
    tnull_closed_group_sum_int32_dot_argument_envs,
    closed_group_direct_column_argument_envs,
    tnull_sum_int32_dot_term, tnull_sum_int32_dot_call,
    direct_column_aggregate_term, AAggregate, Dot.
  reflexivity.
Qed.

Local Lemma tnull_sum_int32_dot_runtime_observations :
  forall group_terms group attribute,
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attribute) =
    NullValues.interp_aggregate_runtime_error
      tnull_sum_int32_dot_call
      (tnull_closed_group_sum_int32_dot_argument_observations
        group_terms group attribute).
Proof.
  intros group_terms group attribute.
  unfold tnull_closed_group_sum_int32_dot_argument_observations,
    tnull_closed_group_sum_int32_dot_argument_envs,
    closed_group_direct_column_argument_observations,
    closed_group_direct_column_argument_envs,
    tnull_sum_int32_dot_term, tnull_sum_int32_dot_call,
    direct_column_aggregate_term, AAggregate, Dot.
  reflexivity.
Qed.

(** Direct evaluator bridge for one closed logical group: value interpreter
    and aggregate-only runtime-error checker for SUM(Int32)→Int64(dot). *)
Theorem tnull_closed_group_sum_int32_dot_value_runtime_exact :
  forall group_terms group attribute,
    group <> nil ->
    Forall (fun row => attribute inS labels TNull row) group ->
    Interp.interp_aggterm TNull
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attribute) =
      NullValues.interp_sum_int32_as_int64
        (map (fun row => dot TNull row attribute) group) /\
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attribute) =
      NullValues.sum_int32_runtime_error
        (map (fun row => dot TNull row attribute) group).
Proof.
  intros group_terms group attribute Hne Hlab.
  pose proof (tnull_closed_group_sum_int32_dot_observations_perm
    group_terms group attribute Hne Hlab) as Hobs.
  assert (Hvalues :
    Permutation
      (tnull_closed_group_sum_int32_dot_argument_values
        group_terms group attribute)
      (map (fun row => dot TNull row attribute) group)).
  {
    rewrite <- tnull_sum_int32_dot_observation_values.
    unfold NullValues.observation_values.
    eapply Permutation_trans with
      (l' := map snd
        (map (fun row => (None, dot TNull row attribute)) group)).
    - apply Permutation_map; exact Hobs.
    - rewrite map_map; apply Permutation_refl.
  }
  assert (Hchildren :
    NullValues.first_observation_error
      (tnull_closed_group_sum_int32_dot_argument_observations
        group_terms group attribute) = None).
  {
    apply first_observation_error_none_iff.
    rewrite Forall_forall; intros observation Hobservation.
    assert (Hmapped :
      In observation
        (map (fun row => (None, dot TNull row attribute)) group)).
    { eapply Permutation_in; [exact Hobs|exact Hobservation]. }
    apply in_map_iff in Hmapped.
    destruct Hmapped as [row [Hequal _]].
    now subst observation.
  }
  split.
  - rewrite tnull_sum_int32_dot_value_observations.
    exact (interp_sum_int32_as_int64_permutation _ _ Hvalues).
  - rewrite tnull_sum_int32_dot_runtime_observations.
    unfold tnull_sum_int32_dot_call.
    rewrite aggregate_call_safe_children_reduce_to_local by exact Hchildren.
    cbn [NullValues.aggregate_local_runtime_error
      NullValues.aggregate_input_values
      NullValues.aggregate_local_runtime_error_function].
    rewrite tnull_sum_int32_dot_observation_values.
    exact (sum_int32_runtime_error_permutation _ _ Hvalues).
Qed.

(** SUM(Int32)(dot attr) leaf exactness over a nonempty closed group. *)
Theorem tnull_sum_int32_dot_leaf_exact :
  forall basesort instance boolean_schedule group_terms group attr,
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    NullValues.sum_int32_runtime_error
      (map (fun row => dot TNull row attr) group) = None ->
    @scalar_value_expr_exact_at TNull relname basesort instance unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      tnull_value_is_null boolean_schedule
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (@SExpr_Leaf TNull relname type_int64
        (tnull_sum_int32_dot_term attr))
      (NullValues.interp_sum_int32_as_int64
        (map (fun row => dot TNull row attr) group)).
Proof.
  intros basesort instance boolean_schedule group_terms group attr
    Hne Hlab Hsafe.
  destruct (@tnull_closed_group_sum_int32_dot_value_runtime_exact
    group_terms group attr Hne Hlab) as [Hval Hagg].
  assert (Hrun :
    @eval_aggterm_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attr) =
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attr)).
  {
    unfold tnull_sum_int32_dot_term, AAggregate.
    reflexivity.
  }
  eapply tnull_aggregate_leaf_exact_of_interp.
  - exact Hval.
  - rewrite Hrun, Hagg; exact Hsafe.
Qed.

Theorem tnull_sum_int32_dot_aggregate_runtime_exact :
  forall group_terms group attr,
    group <> nil ->
    Forall (fun row => attr inS labels TNull row) group ->
    @eval_aggterm_aggregate_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (Env.env_g TNull nil (@Env.Group_By TNull group_terms) group)
      (tnull_sum_int32_dot_term attr) =
    NullValues.sum_int32_runtime_error
      (map (fun row => dot TNull row attr) group).
Proof.
  intros group_terms group attr Hne Hlab.
  exact (proj2 (@tnull_closed_group_sum_int32_dot_value_runtime_exact
    group_terms group attr Hne Hlab)).
Qed.

