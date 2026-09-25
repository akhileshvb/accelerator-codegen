(* Q1 instantiation of FormalSQLLib.QuerySpineFacade (templatized delta).

   Constructor composition is [query_spine_TFGOP_success] (proved once).
   Leaf Props are ∀ [delta]; catalog fill-in is [delta = 90]. *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueTemporal
  ValueNumericTypmod SchemaConstraints
  SqlOutcome SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics
  SqlOrder SqlListFacts FiniteBag FiniteCollection FiniteSet FTuples Bool3
  OrderedSet Env Projection Interp.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.AggregateRuntimeFacts
  FormalSQL.QueryCardinality FormalSQL.NumericRegroupFacts.
From FormalSQLLib Require Import
  QuerySpineFacade FilterAcceptanceFacade ProjectExactFacade
  OrderRowsFacade GroupBagExactFacade GroupsExactFacade
  AggregateLeafExactFacade TemporalPredAcceptanceFacade
  NumericCallExactFacade NumericScaledFits NumericPackagingFacade.
From JetStreamFS Require Import Q1AuxSpec.
From Stdlib Require Import String List ZArith Lia Permutation.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q1Catalog Q1FormalSQLProof Q1GroupingCorrespondence
  Q1AuxCorrectAssembly Q1Obligations.

(** * Nonempty leaf interface (parameterized by interval [delta]) *)

Definition Q1_FilterRowsOk (db : list Q1AuxSpec.Row) (delta : Z)
    (filtered : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil (q1_filter_at delta) (map EmbedRow_Q1 db) (SqlSuccess filtered) /\
    filtered =
      map EmbedRow_Q1
        (filter (fun r =>
           Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta)) db).

Definition Q1_InnerProjectOk (db : list Q1AuxSpec.Row)
    (filtered inner_proj : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q1_inner_select filtered (SqlSuccess inner_proj).

Definition Q1_GroupBagOk (db : list Q1AuxSpec.Row)
    (inner_proj group_rows : list (tuple TNull)) : Prop :=
  forall sched,
    exists group_bag,
      @eval_group_bag_outcome TNull relname
        (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched
        nil q1_group_select q1_group_keys q1_having
        (query_rows_bag inner_proj) (SqlSuccess group_bag) /\
      query_same_rows_as_bag group_rows group_bag.

Definition Q1_OrderOk (group_rows ordered_rows : list (tuple TNull)) : Prop :=
  @order_by_rows TNull value_is_null q1_order_keys group_rows ordered_rows.

Definition Q1_OuterProjectOk (db : list Q1AuxSpec.Row)
    (ordered_rows final_rows : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q1 db)) (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q1_outer_select ordered_rows (SqlSuccess final_rows).

Lemma embed_db_instance_lineitem :
  forall db,
    @_instance TNull (EmbedDB_Q1 db) (Rel "lineitem")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) (map EmbedRow_Q1 db).
Proof.
  intros; reflexivity.
Qed.

Lemma embed_db_table_same_rows :
  forall db,
    query_same_rows_as_bag (map EmbedRow_Q1 db)
      (@query_table_bag TNull relname
         (@_basesort TNull (EmbedDB_Q1 db))
         (@_instance TNull (EmbedDB_Q1 db))
         q1_table_outputs q1_table).
Proof.
  intros db.
  unfold query_same_rows_as_bag.
  rewrite Febag.nb_occ_equal; intro x.
  unfold query_table_bag.
  rewrite embed_db_instance_lineitem.
  destruct (@query_outputs_sort TNull q1_table_outputs =S?=
            @_basesort TNull (EmbedDB_Q1 db) (Rel "lineitem"));
    rewrite ?Febag.nb_occ_mk_bag; reflexivity.
Qed.

(** Facade instantiation: leaves ⇒ membership for any [delta]. Not TCB. *)
Lemma FormalSQLAuxCorrect_Q1_of_leaves :
  forall db delta filtered inner_proj group_rows ordered_rows,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    Q1_FilterRowsOk db delta filtered ->
    Q1_InnerProjectOk db filtered inner_proj ->
    Q1_GroupBagOk db inner_proj group_rows ->
    Q1_OrderOk group_rows ordered_rows ->
    Q1_OuterProjectOk db ordered_rows
      (EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta))) ->
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta))).
Proof.
  intros db delta filtered inner_proj group_rows ordered_rows
    _ _ Hfilt Hinner Hgroup Horder Houter.
  unfold FormalSQL_exec, qhat_Q1.
  exists q1_boolean_schedule.
  destruct (Hgroup q1_boolean_schedule) as [group_bag [Hgeval Hgsame]].
  eapply (@query_spine_TFGOP_success TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      value_is_null
    q1_boolean_schedule).
  - apply embed_db_table_same_rows.
  - apply (proj1 (Hfilt q1_boolean_schedule)).
  - apply (Hinner q1_boolean_schedule).
  - exact Hgeval.
  - exact Hgsame.
  - exact Horder.
  - apply (Houter q1_boolean_schedule).
Qed.

Lemma Q1_FilterRowsOk_nil :
  forall delta,
    Preconditions_Q1 nil delta ->
    Q1_FilterRowsOk nil delta nil.
Proof.
  intros delta _ sched.
  split.
  - apply q1_empty_filter_rows.
  - reflexivity.
Qed.

(** * Thin leaf reductions via FormalSQLLib facades

    Filter / Project / Group remain open at the per-row scalar exactness
    layer.  Do not discharge them by nesting [EFilterRows_Cons] /
    [EProjectRows_Cons] / [EGroups_*] here — apply the facades after proving
    [scalar_expr_acceptance_exact_at] / [scalar_value_list_exact_at] /
    per-bucket aggregate observation. *)

Definition q1_filter_keep (delta : Z) (row : tuple TNull) : bool :=
  match dot TNull row (AttrDate "l_shipdate") with
  | NullValues.Value_date (Some d) => Z.leb d (EmbedParams_Q1 delta)
  | _ => false
  end.

Lemma ShipdateOk_finite_date_timestamp_ok :
  forall d, ShipdateOk_Q1 d -> finite_date_timestamp_ok d.
Proof. intros d H; exact H. Qed.

Lemma Q1_filter_keep_embed_leb :
  forall delta r,
    q1_filter_keep delta (EmbedRow_Q1 r) =
    Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta).
Proof.
  intros delta r.
  unfold q1_filter_keep.
  rewrite EmbedRow_dot_shipdate.
  reflexivity.
Qed.

Lemma Q1_filter_map_embed_commute :
  forall delta db,
    List.filter (q1_filter_keep delta) (map EmbedRow_Q1 db) =
    map EmbedRow_Q1
      (List.filter
        (fun r => Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta)) db).
Proof.
  intros delta db.
  induction db as [|r rs IH]; [reflexivity|].
  cbn [map List.filter].
  rewrite Q1_filter_keep_embed_leb.
  destruct (Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta));
    cbn [map]; rewrite IH; reflexivity.
Qed.

Lemma q1_filter_at_is_date_lte_timestamp_add_pred :
  forall delta,
    q1_filter_at delta =
    date_lte_timestamp_add_pred "l_shipdate" q1_base_date delta.
Proof. reflexivity. Qed.

Lemma q1_base_date_finite_ok : finite_date_timestamp_ok q1_base_date.
Proof.
  unfold q1_base_date, finite_date_timestamp_ok, cast_date_to_timestamp.
  unfold postgres_date_min, postgres_date_end,
    postgres_timestamp_min, postgres_timestamp_end,
    date_is_neg_infinity_bool, date_is_pos_infinity_bool,
    postgres_date_neg_infinity, postgres_date_pos_infinity,
    micros_per_day, micros_per_hour, micros_per_minute, micros_per_second.
  repeat split; lia.
Qed.

Lemma EmbedRow_shipdate_inS :
  forall r,
    AttrDate "l_shipdate" inS labels TNull (EmbedRow_Q1 r).
Proof.
  intro r.
  unfold EmbedRow_Q1.
  match goal with
  | |- context [mk_tuple_lists ?atts ?vals] =>
      rewrite (Fset.mem_eq_2 _ _ _
        (mk_tuple_lists_labels atts vals))
  end.
  exact lineitem_attr_inS_shipdate.
Qed.

Lemma Q1_filter_acceptance_embed :
  forall db delta sched r,
    Preconditions_Q1 db delta ->
    In r db ->
    @scalar_expr_acceptance_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil (EmbedRow_Q1 r)) (q1_filter_at delta)
      (q1_filter_keep delta (EmbedRow_Q1 r)).
Proof.
  intros db delta sched r Hpre Hin.
  destruct Hpre as [_ [Hdelta [Hrows _]]].
  apply Forall_forall with (x := r) in Hrows; [|exact Hin].
  destruct Hrows as [Hship _].
  rewrite Q1_filter_keep_embed_leb.
  unfold EmbedParams_Q1 in Hdelta |- *.
  rewrite q1_filter_at_is_date_lte_timestamp_add_pred.
  eapply date_lte_timestamp_add_pred_acceptance.
  - apply EmbedRow_shipdate_inS.
  - apply EmbedRow_dot_shipdate.
  - apply ShipdateOk_finite_date_timestamp_ok; exact Hship.
  - exact q1_base_date_finite_ok.
  - apply ShipdateOk_finite_date_timestamp_ok; exact Hdelta.
Qed.

(** FilterRowsOk from per-row acceptance (FilterAcceptanceFacade). *)
Lemma Q1_FilterRowsOk_of_acceptance :
  forall db delta,
    Preconditions_Q1 db delta ->
    (forall sched row,
      In row (map EmbedRow_Q1 db) ->
      @scalar_expr_acceptance_exact_at TNull relname
        (@_basesort TNull (EmbedDB_Q1 db))
        (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched
        (Env.env_t TNull nil row) (q1_filter_at delta)
        (q1_filter_keep delta row)) ->
    Q1_FilterRowsOk db delta
      (map EmbedRow_Q1
        (List.filter
          (fun r => Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta))
          db)).
Proof.
  intros db delta _ Hacc sched.
  split.
  - rewrite <- Q1_filter_map_embed_commute.
    eapply (@tnull_filter_rows_success_of_acceptance
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db)) sched).
    intros row Hin.
    apply (Hacc sched row Hin).
  - reflexivity.
Qed.

(** FilterRowsOk — discharged via TemporalPredAcceptanceFacade + Embed + Pre. *)
Lemma Q1_FilterRowsOk_holds :
  forall db delta,
    Preconditions_Q1 db delta ->
    Q1_FilterRowsOk db delta
      (map EmbedRow_Q1
        (List.filter
          (fun r => Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta))
          db)).
Proof.
  intros db delta Hpre.
  apply Q1_FilterRowsOk_of_acceptance; [exact Hpre|].
  intros sched row Hin.
  apply in_map_iff in Hin.
  destruct Hin as [r [Heq Hinr]].
  subst row.
  eapply Q1_filter_acceptance_embed; eassumption.
Qed.

(** Inner/outer Project from per-row value-list exactness (ProjectExactFacade).
    Remaining obligation: discharge [scalar_value_list_exact_at] on the select
    list (inner: dots + $f4/$f5; outer: group output dots). *)
Lemma Q1_InnerProjectOk_of_exact_map :
  forall db filtered
      (values : tuple TNull -> list NullValues.value),
    (forall sched row,
      In row filtered ->
      @scalar_value_list_exact_at TNull relname
        (@_basesort TNull (EmbedDB_Q1 db))
        (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched
        (Env.env_t TNull nil row) (map fst q1_inner_select) (values row)) ->
    Q1_InnerProjectOk db filtered
      (map (fun row => project_row q1_inner_select (values row)) filtered).
Proof.
  intros db filtered values Hexact sched.
  eapply (@tnull_project_rows_success_of_exact_map
    (@_basesort TNull (EmbedDB_Q1 db))
    (@_instance TNull (EmbedDB_Q1 db)) sched).
  intros row Hin; apply (Hexact sched row Hin).
Qed.

Lemma Q1_OuterProjectOk_of_exact_map :
  forall db ordered_rows
      (values : tuple TNull -> list NullValues.value),
    (forall sched row,
      In row ordered_rows ->
      @scalar_value_list_exact_at TNull relname
        (@_basesort TNull (EmbedDB_Q1 db))
        (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched
        (Env.env_t TNull nil row) (map fst q1_outer_select) (values row)) ->
    Q1_OuterProjectOk db ordered_rows
      (map (fun row => project_row q1_outer_select (values row)) ordered_rows).
Proof.
  intros db ordered_rows values Hexact sched.
  eapply (@tnull_project_rows_success_of_exact_map
    (@_basesort TNull (EmbedDB_Q1 db))
    (@_instance TNull (EmbedDB_Q1 db)) sched).
  intros row Hin; apply (Hexact sched row Hin).
Qed.

(** GroupBagOk from exact groups observation (GroupBagExactFacade).
    Prefer [Q1_GroupBagOk_of_exact_filter_map] / having-true below so
    query files never nest [EGroups_*]. *)
Lemma Q1_GroupBagOk_of_exact_groups :
  forall db inner_proj group_terms grouped_rows,
    scalar_group_key_terms q1_group_keys = Some group_terms ->
    (forall sched,
      @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
        nil group_terms inner_proj = None /\
      @eval_groups_outcome TNull relname
        (@_basesort TNull (EmbedDB_Q1 db))
        (@_instance TNull (EmbedDB_Q1 db))
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error value_is_null sched
        nil q1_group_select group_terms q1_having
        (query_make_groups nil inner_proj group_terms)
        (SqlSuccess grouped_rows)) ->
    Q1_GroupBagOk db inner_proj grouped_rows.
Proof.
  intros db inner_proj group_terms grouped_rows Hterms Hexact sched.
  exists (query_rows_bag grouped_rows).
  destruct (Hexact sched) as [Hkeys Hgroups].
  split.
  - eapply (@tnull_group_bag_success_of_exact_groups
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db)) sched).
    + exact Hterms.
    + unfold query_same_rows_as_bag; apply Febag.equal_refl.
    + exact Hkeys.
    + exact Hgroups.
  - apply Febag.equal_refl.
Qed.

(** GroupBagOk from per-bucket [group_filter_map_exact_at] (GroupsExactFacade).
    Discharge HAVING/select exactness per group via AggregateLeafExactFacade
    (COUNT-star / SUM(dot) / AVG(fixed)(dot)). *)
Lemma Q1_GroupBagOk_of_exact_filter_map :
  forall db inner_proj group_terms keep values,
    scalar_group_key_terms q1_group_keys = Some group_terms ->
    (forall sched,
      @group_keys_runtime_error TNull
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
        nil group_terms inner_proj = None /\
      let groups := query_make_groups nil inner_proj group_terms in
      forall group,
        In group groups ->
        @group_filter_map_exact_at TNull relname
          (@_basesort TNull (EmbedDB_Q1 db))
          (@_instance TNull (EmbedDB_Q1 db))
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil q1_group_select group_terms q1_having keep values group) ->
    Q1_GroupBagOk db inner_proj
      (map (fun group => project_row q1_group_select (values group))
        (filter keep (query_make_groups nil inner_proj group_terms))).
Proof.
  intros db inner_proj group_terms keep values Hterms Hexact sched.
  exists
    (query_rows_bag
      (map (fun group => project_row q1_group_select (values group))
        (filter keep (query_make_groups nil inner_proj group_terms)))).
  destruct (Hexact sched) as [Hkeys Hgroups].
  split.
  - eapply (@tnull_group_bag_success_of_exact_filter_map
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db)) sched).
    + exact Hterms.
    + unfold query_same_rows_as_bag; apply Febag.equal_refl.
    + exact Hkeys.
    + exact Hgroups.
  - apply Febag.equal_refl.
Qed.

Lemma Q1_GroupBagOk_of_exact_map_having_true :
  forall db inner_proj group_terms values,
    scalar_group_key_terms q1_group_keys = Some group_terms ->
    (forall sched,
      @group_keys_runtime_error TNull
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        nil group_terms inner_proj = None /\
      let groups := query_make_groups nil inner_proj group_terms in
      forall group,
        In group groups ->
        @group_filter_map_exact_at TNull relname
          (@_basesort TNull (EmbedDB_Q1 db))
          (@_instance TNull (EmbedDB_Q1 db))
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error value_is_null sched
          nil q1_group_select group_terms q1_having
          (fun _ => true) values group) ->
    Q1_GroupBagOk db inner_proj
      (map (fun group => project_row q1_group_select (values group))
        (query_make_groups nil inner_proj group_terms)).
Proof.
  intros db inner_proj group_terms values Hterms Hexact.
  replace
    (map (fun group => project_row q1_group_select (values group))
      (query_make_groups nil inner_proj group_terms))
    with
      (map (fun group => project_row q1_group_select (values group))
        (filter (fun _ => true)
          (query_make_groups nil inner_proj group_terms)))
    by (rewrite filter_keep_all_id; reflexivity).
  apply Q1_GroupBagOk_of_exact_filter_map with
    (keep := fun _ => true) (values := values).
  - exact Hterms.
  - exact Hexact.
Qed.

Lemma q1_group_keys_terms :
  scalar_group_key_terms q1_group_keys = Some q1_group_terms.
Proof.
  unfold q1_group_keys, q1_group_terms; reflexivity.
Qed.

Lemma q1_having_is_true :
  q1_having = @SExpr_True TNull relname.
Proof.
  unfold q1_having, Queries.scalar_expr_predicate_0; reflexivity.
Qed.

Lemma q1_group_dot_keys_runtime_none :
  forall rows,
    @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      nil q1_group_terms rows = None.
Proof.
  induction rows as [|row rows IH]; [reflexivity|].
  cbn [group_keys_runtime_error first_runtime_error].
  (* Both keys are DotString leaves: funterm runtime is None. *)
  unfold q1_group_terms.
  cbn [eval_aggterm_runtime_error eval_funterm_runtime_error].
  exact IH.
Qed.

Lemma Q1_GroupBagOk_nil :
  forall db, Q1_GroupBagOk db nil nil.
Proof.
  intros db.
  apply Q1_GroupBagOk_of_exact_groups with (group_terms := q1_group_terms).
  - exact q1_group_keys_terms.
  - intros sched; split.
    + apply q1_group_dot_keys_runtime_none.
    + rewrite query_make_groups_empty_shape; cbn.
      apply EGroups_Nil.
Qed.

(** OrderOk — fully discharged by OrderRowsFacade (canonical sort). *)
Lemma Q1_OrderOk_canonical :
  forall group_rows,
    Q1_OrderOk group_rows
      (@sort_ordered_rows TNull value_is_null q1_order_keys group_rows).
Proof.
  intros group_rows.
  unfold Q1_OrderOk.
  apply (@order_by_rows_canonical TNull value_is_null).
Qed.

Lemma Q1_OrderOk_exists :
  forall group_rows,
    exists ordered_rows, Q1_OrderOk group_rows ordered_rows.
Proof.
  intros group_rows.
  exists (@sort_ordered_rows TNull value_is_null q1_order_keys group_rows).
  apply Q1_OrderOk_canonical.
Qed.

Lemma Q1_OrderOk_nil :
  Q1_OrderOk nil nil.
Proof.
  exact q1_empty_order_rows.
Qed.

(** * Outer project — pure Dot leaves of group-output attributes *)

Definition q1_outer_dot_values (row : tuple TNull) : list NullValues.value :=
  [ dot TNull row (AttrString "l_returnflag" (StringChar 1))
  ; dot TNull row (AttrString "l_linestatus" (StringChar 1))
  ; dot TNull row (AttrNumeric "sum_qty")
  ; dot TNull row (AttrNumeric "sum_base_price")
  ; dot TNull row (AttrNumeric "sum_disc_price")
  ; dot TNull row (AttrNumeric "sum_charge")
  ; dot TNull row (AttrNumeric "avg_qty")
  ; dot TNull row (AttrNumeric "avg_price")
  ; dot TNull row (AttrNumeric "avg_disc")
  ; dot TNull row (AttrInt64 "count_order")
  ].

Definition q1_outer_row_labels_ok (row : tuple TNull) : Prop :=
  AttrString "l_returnflag" (StringChar 1) inS labels TNull row /\
  AttrString "l_linestatus" (StringChar 1) inS labels TNull row /\
  AttrNumeric "sum_qty" inS labels TNull row /\
  AttrNumeric "sum_base_price" inS labels TNull row /\
  AttrNumeric "sum_disc_price" inS labels TNull row /\
  AttrNumeric "sum_charge" inS labels TNull row /\
  AttrNumeric "avg_qty" inS labels TNull row /\
  AttrNumeric "avg_price" inS labels TNull row /\
  AttrNumeric "avg_disc" inS labels TNull row /\
  AttrInt64 "count_order" inS labels TNull row.

Lemma q1_outer_select_exact_of_dots :
  forall db sched row,
    q1_outer_row_labels_ok row ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil row) (map fst q1_outer_select)
      (q1_outer_dot_values row).
Proof.
  intros db sched row Hlab.
  destruct Hlab as [Hrf [Hls [Hsq [Hsb [Hsd [Hsc [Haq [Hap [Had Hco]]]]]]]]].
  unfold q1_outer_select, Queries.scalar_select_list_0, q1_outer_dot_values.
  cbn [map fst].
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull (EmbedDB_Q1 db))
    (@_instance TNull (EmbedDB_Q1 db))
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched)).
  - apply tnull_dot_string_exact.
    apply interp_dot_env_t; exact Hrf.
  - apply tnull_dot_string_exact.
    apply interp_dot_env_t; exact Hls.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Hsq.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Hsb.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Hsd.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Hsc.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Haq.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Hap.
  - apply tnull_dot_numeric_exact.
    apply interp_dot_env_t; exact Had.
  - apply tnull_dot_int64_exact.
    apply interp_dot_env_t; exact Hco.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched).
Qed.

(** OuterProjectOk — discharged via ProjectExactFacade + Dot leaf exact. *)
Lemma Q1_OuterProjectOk_holds :
  forall db ordered_rows,
    (forall row, In row ordered_rows -> q1_outer_row_labels_ok row) ->
    Q1_OuterProjectOk db ordered_rows
      (map (fun row => project_row q1_outer_select (q1_outer_dot_values row))
        ordered_rows).
Proof.
  intros db ordered_rows Hlab.
  apply Q1_OuterProjectOk_of_exact_map with (values := q1_outer_dot_values).
  intros sched row Hin.
  apply q1_outer_select_exact_of_dots.
  apply Hlab; exact Hin.
Qed.

Lemma Q1_OuterProjectOk_nil :
  forall db, Q1_OuterProjectOk db nil nil.
Proof.
  intros db.
  change nil with
    (map (fun row => project_row q1_outer_select (q1_outer_dot_values row)) nil).
  apply Q1_OuterProjectOk_holds.
  intros row Hin; contradiction.
Qed.

(** * Inner project — dots + $f4/$f5 via NumericCallExactFacade *)

Definition q1_f4_num (r : Q1AuxSpec.Row) : numeric :=
  numeric_mul (numeric_of_scaled (Q1AuxSpec.price r) 2)
    (numeric_sub (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.disc r) 2)).

Definition q1_f5_num (r : Q1AuxSpec.Row) : numeric :=
  numeric_mul (q1_f4_num r)
    (numeric_add (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.tax r) 2)).

Definition q1_inner_values (r : Q1AuxSpec.Row) : list NullValues.value :=
  [ embed_flag_char (Q1AuxSpec.rf r)
  ; embed_flag_char (Q1AuxSpec.ls r)
  ; embed_decimal2 (Q1AuxSpec.qty r)
  ; embed_decimal2 (Q1AuxSpec.price r)
  ; NullValues.Value_numeric (Some (q1_f4_num r))
  ; NullValues.Value_numeric (Some (q1_f5_num r))
  ; embed_decimal2 (Q1AuxSpec.disc r)
  ].

Lemma EmbedRow_attr_inS_generic :
  forall r (attr : attribute TNull),
    attr inS Fset.mk_set (A TNull) lineitem_attrs ->
    attr inS labels TNull (EmbedRow_Q1 r).
Proof.
  intros r attr Hin.
  unfold EmbedRow_Q1.
  match goal with
  | |- context [mk_tuple_lists ?atts ?vals] =>
      rewrite (Fset.mem_eq_2 _ _ _
        (mk_tuple_lists_labels atts vals))
  end.
  exact Hin.
Qed.

Lemma q1_inner_select_is_disc_price_charge_shape :
  map fst q1_inner_select =
  [ @SExpr_Leaf TNull relname type_string
      (DotString "l_returnflag" (StringChar 1))
  ; @SExpr_Leaf TNull relname type_string
      (DotString "l_linestatus" (StringChar 1))
  ; @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2)
  ; @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2)
  ; numeric_disc_price_expr "l_extendedprice" "l_discount" 15 2
  ; numeric_charge_expr "l_extendedprice" "l_discount" "l_tax" 15 2
  ; @SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2)
  ].
Proof.
  unfold q1_inner_select, Queries.scalar_select_list_2.
  unfold numeric_disc_price_expr, numeric_charge_expr,
    numeric_one_minus_disc_expr, numeric_one_plus_tax_expr.
  reflexivity.
Qed.

Lemma q1_row_disc_tax_factor_fits :
  forall r, RowOk_Q1 r ->
    numeric_runtime_fits_bool
      (numeric_sub (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.disc r) 2))
      = true /\
    numeric_runtime_fits_bool
      (numeric_add (numeric_of_Z 1) (numeric_of_scaled (Q1AuxSpec.tax r) 2))
      = true.
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [_ [_ [Hd Ht]]]]]].
  apply numeric_disc_tax_factor_fits_tpch_q1; assumption.
Qed.

(** Mul-tree fits for [$f4]/[$f5] at the domain-max price (digit-budget witness).
    Lifting from max price to an arbitrary in-domain [price] is the remaining
    Inner obligation (magnitude-monotone digit budget). *)
Lemma q1_row_f4_f5_fits_max_price :
  forall r, RowOk_Q1 r ->
    numeric_runtime_fits_bool
      (numeric_mul (numeric_of_scaled 1000000000000 2)
        (numeric_sub (numeric_of_Z 1)
          (numeric_of_scaled (Q1AuxSpec.disc r) 2))) = true /\
    numeric_runtime_fits_bool
      (numeric_mul
        (numeric_mul (numeric_of_scaled 1000000000000 2)
          (numeric_sub (numeric_of_Z 1)
            (numeric_of_scaled (Q1AuxSpec.disc r) 2)))
        (numeric_add (numeric_of_Z 1)
          (numeric_of_scaled (Q1AuxSpec.tax r) 2))) = true.
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [_ [_ [Hd Ht]]]]]].
  apply numeric_disc_price_charge_fits_max_price_tpch_q1; assumption.
Qed.

Lemma q1_row_f4_f5_fits :
  forall r, RowOk_Q1 r ->
    numeric_runtime_fits_bool (q1_f4_num r) = true /\
    numeric_runtime_fits_bool (q1_f5_num r) = true.
Proof.
  intros r Hrow.
  destruct (q1_row_f4_f5_fits_max_price r Hrow) as [Hf4 Hf5].
  destruct Hrow as [_ [_ [_ [_ [Hp [Hd Ht]]]]]].
  unfold q1_f4_num, q1_f5_num.
  eapply numeric_disc_price_charge_fits_mono_tpch_q1
    with (Pmax := 1000000000000); eassumption.
Qed.

Definition q1_inner_values_embed (row : tuple TNull) : list NullValues.value :=
  match
    dot TNull row (AttrDecimal "l_extendedprice" 15 2),
    dot TNull row (AttrDecimal "l_discount" 15 2),
    dot TNull row (AttrDecimal "l_tax" 15 2)
  with
  | NullValues.Value_numeric (Some price_n),
    NullValues.Value_numeric (Some disc_n),
    NullValues.Value_numeric (Some tax_n) =>
      [ dot TNull row (AttrString "l_returnflag" (StringChar 1))
      ; dot TNull row (AttrString "l_linestatus" (StringChar 1))
      ; dot TNull row (AttrDecimal "l_quantity" 15 2)
      ; NullValues.Value_numeric (Some price_n)
      ; NullValues.Value_numeric
          (Some (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n)))
      ; NullValues.Value_numeric
          (Some (numeric_mul
            (numeric_mul price_n (numeric_sub (numeric_of_Z 1) disc_n))
            (numeric_add (numeric_of_Z 1) tax_n)))
      ; NullValues.Value_numeric (Some disc_n)
      ]
  | _, _, _ => nil
  end.

Lemma q1_inner_values_embed_of_row :
  forall r,
    q1_inner_values_embed (EmbedRow_Q1 r) = q1_inner_values r.
Proof.
  intros r.
  unfold q1_inner_values_embed, q1_inner_values, q1_f4_num, q1_f5_num.
  rewrite EmbedRow_dot_price, EmbedRow_dot_disc, EmbedRow_dot_tax.
  rewrite EmbedRow_dot_rf, EmbedRow_dot_ls, EmbedRow_dot_qty.
  reflexivity.
Qed.

Lemma q1_dot_price_exact :
  forall db sched r,
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil (EmbedRow_Q1 r))
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_extendedprice" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.price r) 2))).
Proof.
  intros db sched r.
  apply tnull_dot_decimal_exact.
  change (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.price r) 2)))
    with (embed_decimal2 (Q1AuxSpec.price r)).
  rewrite <- EmbedRow_dot_price.
  apply interp_dot_env_t.
  apply EmbedRow_attr_inS_generic, lineitem_attr_inS_price.
Qed.

Lemma q1_dot_disc_exact :
  forall db sched r,
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil (EmbedRow_Q1 r))
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_discount" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.disc r) 2))).
Proof.
  intros db sched r.
  apply tnull_dot_decimal_exact.
  change (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.disc r) 2)))
    with (embed_decimal2 (Q1AuxSpec.disc r)).
  rewrite <- EmbedRow_dot_disc.
  apply interp_dot_env_t.
  apply EmbedRow_attr_inS_generic, lineitem_attr_inS_disc.
Qed.

Lemma q1_dot_tax_exact :
  forall db sched r,
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil (EmbedRow_Q1 r))
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_tax" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.tax r) 2))).
Proof.
  intros db sched r.
  apply tnull_dot_decimal_exact.
  change (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.tax r) 2)))
    with (embed_decimal2 (Q1AuxSpec.tax r)).
  rewrite <- EmbedRow_dot_tax.
  apply interp_dot_env_t.
  apply EmbedRow_attr_inS_generic, lineitem_attr_inS_tax.
Qed.

Lemma q1_dot_qty_exact :
  forall db sched r,
    @scalar_value_expr_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil (EmbedRow_Q1 r))
      (@SExpr_Leaf TNull relname type_numeric (DotDecimal "l_quantity" 15 2))
      (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.qty r) 2))).
Proof.
  intros db sched r.
  apply tnull_dot_decimal_exact.
  change (NullValues.Value_numeric (Some (numeric_of_scaled (Q1AuxSpec.qty r) 2)))
    with (embed_decimal2 (Q1AuxSpec.qty r)).
  rewrite <- EmbedRow_dot_qty.
  apply interp_dot_env_t.
  apply EmbedRow_attr_inS_generic, lineitem_attr_inS_qty.
Qed.

Lemma q1_inner_select_exact :
  forall db sched r,
    RowOk_Q1 r ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (Env.env_t TNull nil (EmbedRow_Q1 r))
      (map fst q1_inner_select)
      (q1_inner_values r).
Proof.
  intros db sched r Hrow.
  destruct (q1_row_disc_tax_factor_fits r Hrow) as [Hsub Hadd].
  destruct (q1_row_f4_f5_fits r Hrow) as [Hf4 Hf5].
  rewrite q1_inner_select_is_disc_price_charge_shape.
  unfold q1_inner_values, q1_f4_num, q1_f5_num.
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull (EmbedDB_Q1 db))
    (@_instance TNull (EmbedDB_Q1 db))
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched)).
  - apply tnull_dot_string_exact.
    apply interp_dot_env_t.
    apply EmbedRow_attr_inS_generic, lineitem_attr_inS_rf.
  - apply tnull_dot_string_exact.
    apply interp_dot_env_t.
    apply EmbedRow_attr_inS_generic, lineitem_attr_inS_ls.
  - apply q1_dot_qty_exact.
  - apply q1_dot_price_exact.
  - eapply tnull_disc_price_exact.
    + apply q1_dot_price_exact.
    + apply q1_dot_disc_exact.
    + exact Hsub.
    + exact Hf4.
  - eapply tnull_charge_exact.
    + apply q1_dot_price_exact.
    + apply q1_dot_disc_exact.
    + apply q1_dot_tax_exact.
    + exact Hsub.
    + exact Hadd.
    + exact Hf4.
    + exact Hf5.
  - apply q1_dot_disc_exact.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched).
Qed.

Lemma Q1_InnerProjectOk_holds :
  forall db rs,
    Forall RowOk_Q1 rs ->
    Q1_InnerProjectOk db (map EmbedRow_Q1 rs)
      (map (fun r => project_row q1_inner_select (q1_inner_values r)) rs).
Proof.
  intros db rs Hok.
  assert (Hmap :
    map (fun r => project_row q1_inner_select (q1_inner_values r)) rs =
    map (fun row => project_row q1_inner_select (q1_inner_values_embed row))
      (map EmbedRow_Q1 rs)).
  { rewrite map_map.
    apply map_ext; intros r.
    rewrite q1_inner_values_embed_of_row.
    reflexivity. }
  rewrite Hmap.
  apply Q1_InnerProjectOk_of_exact_map with (values := q1_inner_values_embed).
  intros sched row Hin.
  apply in_map_iff in Hin.
  destruct Hin as [r [Heq Hinr]].
  subst row.
  rewrite q1_inner_values_embed_of_row.
  apply q1_inner_select_exact.
  eapply Forall_forall; eassumption.
Qed.

Lemma Q1_InnerProjectOk_nil :
  forall db, Q1_InnerProjectOk db nil nil.
Proof.
  intros db.
  change (Q1_InnerProjectOk db nil nil) with
    (Q1_InnerProjectOk db (map EmbedRow_Q1 nil)
      (map (fun r => project_row q1_inner_select (q1_inner_values r)) nil)).
  apply Q1_InnerProjectOk_holds.
  constructor.
Qed.

(** * GroupBagOk holds — thin instantiation of GroupsExact + AggregateLeaf *)

Definition q1_attr_rf : attribute TNull :=
  AttrString "l_returnflag" (StringChar 1).
Definition q1_attr_ls : attribute TNull :=
  AttrString "l_linestatus" (StringChar 1).
Definition q1_attr_qty : attribute TNull := AttrDecimal "l_quantity" 15 2.
Definition q1_attr_price : attribute TNull := AttrDecimal "l_extendedprice" 15 2.
Definition q1_attr_disc : attribute TNull := AttrDecimal "l_discount" 15 2.
Definition q1_attr_f4 : attribute TNull := AttrNumeric "$f4".
Definition q1_attr_f5 : attribute TNull := AttrNumeric "$f5".

Definition q1_group_env (group : list (tuple TNull)) : Env.env TNull :=
  Env.env_g TNull nil (@Env.Group_By TNull q1_group_terms) group.

(** Select-value observation for one closed group (SUM/AVG/COUNT + key dots). *)
Definition q1_group_select_values (group : list (tuple TNull))
  : list NullValues.value :=
  [ Interp.interp_dot TNull (q1_group_env group) q1_attr_rf
  ; Interp.interp_dot TNull (q1_group_env group) q1_attr_ls
  ; NullValues.interp_sum_numeric
      (map (fun row => dot TNull row q1_attr_qty) group)
  ; NullValues.interp_sum_numeric
      (map (fun row => dot TNull row q1_attr_price) group)
  ; NullValues.interp_sum_numeric
      (map (fun row => dot TNull row q1_attr_f4) group)
  ; NullValues.interp_sum_numeric
      (map (fun row => dot TNull row q1_attr_f5) group)
  ; NullValues.interp_avg_numeric_fixed 15 2
      (map (fun row => dot TNull row q1_attr_qty) group)
  ; NullValues.interp_avg_numeric_fixed 15 2
      (map (fun row => dot TNull row q1_attr_price) group)
  ; NullValues.interp_avg_numeric_fixed 15 2
      (map (fun row => dot TNull row q1_attr_disc) group)
  ; NullValues.value_int64_checked (Z.of_nat (List.length group))
  ].

(** Labels required on every row of a group bucket (inner-project schema). *)
Definition q1_group_row_labels_ok (row : tuple TNull) : Prop :=
  q1_attr_rf inS labels TNull row /\
  q1_attr_ls inS labels TNull row /\
  q1_attr_qty inS labels TNull row /\
  q1_attr_price inS labels TNull row /\
  q1_attr_disc inS labels TNull row /\
  q1_attr_f4 inS labels TNull row /\
  q1_attr_f5 inS labels TNull row.

(** Per-bucket overflow packaging (Q1NumericSafe-style; discharge via TCB). *)
Definition q1_group_bucket_safe (group : list (tuple TNull)) : Prop :=
  NullValues.sum_numeric_runtime_error
    (map (fun row => dot TNull row q1_attr_qty) group) = None /\
  NullValues.sum_numeric_runtime_error
    (map (fun row => dot TNull row q1_attr_price) group) = None /\
  NullValues.sum_numeric_runtime_error
    (map (fun row => dot TNull row q1_attr_f4) group) = None /\
  NullValues.sum_numeric_runtime_error
    (map (fun row => dot TNull row q1_attr_f5) group) = None /\
  NullValues.avg_numeric_fixed_runtime_error 15 2
    (map (fun row => dot TNull row q1_attr_qty) group) = None /\
  NullValues.avg_numeric_fixed_runtime_error 15 2
    (map (fun row => dot TNull row q1_attr_price) group) = None /\
  NullValues.avg_numeric_fixed_runtime_error 15 2
    (map (fun row => dot TNull row q1_attr_disc) group) = None /\
  NullValues.int64_result_runtime_error (Z.of_nat (List.length group)) = None.

Lemma q1_group_row_labels_Forall_attr :
  forall attr group,
    Forall q1_group_row_labels_ok group ->
    (forall row, q1_group_row_labels_ok row -> attr inS labels TNull row) ->
    Forall (fun row => attr inS labels TNull row) group.
Proof.
  intros attr group Hlab Hattr.
  eapply Forall_impl; [|exact Hlab].
  intros row Hrow; apply Hattr; exact Hrow.
Qed.

Lemma q1_group_select_list_exact :
  forall db sched group,
    group <> nil ->
    Forall q1_group_row_labels_ok group ->
    q1_group_bucket_safe group ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      (q1_group_env group) (map fst q1_group_select)
      (q1_group_select_values group).
Proof.
  intros db sched group Hne Hlab Hsafe.
  destruct Hsafe as [Hsum_qty [Hsum_price [Hsum_f4 [Hsum_f5
    [Havg_qty [Havg_price [Havg_disc Hcount]]]]]]].
  assert (Hqty :
    Forall (fun row => q1_attr_qty inS labels TNull row) group).
  { eapply q1_group_row_labels_Forall_attr; [exact Hlab|].
    intros row H; apply H. }
  assert (Hprice :
    Forall (fun row => q1_attr_price inS labels TNull row) group).
  { eapply q1_group_row_labels_Forall_attr; [exact Hlab|].
    intros row H; apply H. }
  assert (Hf4 :
    Forall (fun row => q1_attr_f4 inS labels TNull row) group).
  { eapply q1_group_row_labels_Forall_attr; [exact Hlab|].
    intros row H; apply H. }
  assert (Hf5 :
    Forall (fun row => q1_attr_f5 inS labels TNull row) group).
  { eapply q1_group_row_labels_Forall_attr; [exact Hlab|].
    intros row H; apply H. }
  assert (Hdisc :
    Forall (fun row => q1_attr_disc inS labels TNull row) group).
  { eapply q1_group_row_labels_Forall_attr; [exact Hlab|].
    intros row H; apply H. }
  unfold q1_group_select, Queries.scalar_select_list_1,
    q1_group_select_values, q1_group_env.
  cbn [map fst].
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull (EmbedDB_Q1 db))
    (@_instance TNull (EmbedDB_Q1 db))
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error value_is_null sched)).
  - apply tnull_dot_string_exact; reflexivity.
  - apply tnull_dot_string_exact; reflexivity.
  - eapply tnull_sum_numeric_dot_leaf_exact; eassumption.
  - eapply tnull_sum_numeric_dot_leaf_exact; eassumption.
  - eapply tnull_sum_numeric_dot_leaf_exact; eassumption.
  - eapply tnull_sum_numeric_dot_leaf_exact; eassumption.
  - eapply tnull_avg_numeric_fixed_dot_leaf_exact; eassumption.
  - eapply tnull_avg_numeric_fixed_dot_leaf_exact; eassumption.
  - eapply tnull_avg_numeric_fixed_dot_leaf_exact; eassumption.
  - eapply tnull_count_star_leaf_exact; eassumption.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched).
Qed.

Lemma q1_group_select_aggregate_runtime_none :
  forall group,
    group <> nil ->
    Forall q1_group_row_labels_ok group ->
    q1_group_bucket_safe group ->
    @eval_scalar_select_aggregate_runtime_error TNull relname
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      (q1_group_env group) q1_group_select = None.
Proof.
  intros group Hne Hlab Hsafe.
  destruct Hsafe as [Hsum_qty [Hsum_price [Hsum_f4 [Hsum_f5
    [Havg_qty [Havg_price [Havg_disc Hcount]]]]]]].
  assert (Hqty :
    Forall (fun row => q1_attr_qty inS labels TNull row) group)
    by (eapply q1_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  assert (Hprice :
    Forall (fun row => q1_attr_price inS labels TNull row) group)
    by (eapply q1_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  assert (Hf4 :
    Forall (fun row => q1_attr_f4 inS labels TNull row) group)
    by (eapply q1_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  assert (Hf5 :
    Forall (fun row => q1_attr_f5 inS labels TNull row) group)
    by (eapply q1_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  assert (Hdisc :
    Forall (fun row => q1_attr_disc inS labels TNull row) group)
    by (eapply q1_group_row_labels_Forall_attr; [exact Hlab|];
      intros row H; apply H).
  unfold q1_group_select, Queries.scalar_select_list_1, q1_group_env.
  unfold eval_scalar_select_aggregate_runtime_error.
  apply (proj2 (first_runtime_error_none_iff _ _ _)).
  (* Two Dot key leaves: aggregate finalize is definitionally None. *)
  do 2 (constructor; [simpl fst; reflexivity|]).
  constructor; [|constructor; [|constructor; [|constructor; [|
  constructor; [|constructor; [|constructor; [|constructor; [|constructor]]]]]]]].
  - simpl fst.
    change (AAggregate AggregateSumNumeric AggregateAll
      (Dot (AttrDecimal "l_quantity" 15 2)))
      with (tnull_sum_numeric_dot_term q1_attr_qty).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_sum_numeric_dot_aggregate_runtime_exact
      q1_group_terms group q1_attr_qty Hne Hqty); exact Hsum_qty.
  - simpl fst.
    change (AAggregate AggregateSumNumeric AggregateAll
      (Dot (AttrDecimal "l_extendedprice" 15 2)))
      with (tnull_sum_numeric_dot_term q1_attr_price).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_sum_numeric_dot_aggregate_runtime_exact
      q1_group_terms group q1_attr_price Hne Hprice); exact Hsum_price.
  - simpl fst.
    change (AAggregate AggregateSumNumeric AggregateAll
      (Dot (AttrNumeric "$f4")))
      with (tnull_sum_numeric_dot_term q1_attr_f4).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_sum_numeric_dot_aggregate_runtime_exact
      q1_group_terms group q1_attr_f4 Hne Hf4); exact Hsum_f4.
  - simpl fst.
    change (AAggregate AggregateSumNumeric AggregateAll
      (Dot (AttrNumeric "$f5")))
      with (tnull_sum_numeric_dot_term q1_attr_f5).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_sum_numeric_dot_aggregate_runtime_exact
      q1_group_terms group q1_attr_f5 Hne Hf5); exact Hsum_f5.
  - simpl fst.
    change (AAggregate (AggregateAverageNumericFixed 15 2) AggregateAll
      (Dot (AttrDecimal "l_quantity" 15 2)))
      with (tnull_avg_numeric_fixed_dot_term 15 2 q1_attr_qty).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_avg_numeric_fixed_dot_aggregate_runtime_exact
      15 2 q1_group_terms group q1_attr_qty Hne Hqty); exact Havg_qty.
  - simpl fst.
    change (AAggregate (AggregateAverageNumericFixed 15 2) AggregateAll
      (Dot (AttrDecimal "l_extendedprice" 15 2)))
      with (tnull_avg_numeric_fixed_dot_term 15 2 q1_attr_price).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_avg_numeric_fixed_dot_aggregate_runtime_exact
      15 2 q1_group_terms group q1_attr_price Hne Hprice); exact Havg_price.
  - simpl fst.
    change (AAggregate (AggregateAverageNumericFixed 15 2) AggregateAll
      (Dot (AttrDecimal "l_discount" 15 2)))
      with (tnull_avg_numeric_fixed_dot_term 15 2 q1_attr_disc).
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_avg_numeric_fixed_dot_aggregate_runtime_exact
      15 2 q1_group_terms group q1_attr_disc Hne Hdisc); exact Havg_disc.
  - simpl fst.
    unfold eval_scalar_expr_aggregate_runtime_error.
    rewrite (tnull_count_star_aggregate_runtime_exact
      nil q1_group_terms group); exact Hcount.
Qed.

Lemma q1_group_filter_map_exact :
  forall db sched group,
    group <> nil ->
    Forall q1_group_row_labels_ok group ->
    q1_group_bucket_safe group ->
    @group_filter_map_exact_at TNull relname
      (@_basesort TNull (EmbedDB_Q1 db))
      (@_instance TNull (EmbedDB_Q1 db))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q1_group_select q1_group_terms q1_having
      (fun _ => true) q1_group_select_values group.
Proof.
  intros db sched group Hne Hlab Hsafe.
  unfold group_filter_map_exact_at.
  split.
  - apply q1_group_select_aggregate_runtime_none; assumption.
  - split.
    + rewrite q1_having_is_true; reflexivity.
    + split.
      * rewrite q1_having_is_true.
        eapply (@tnull_having_true_acceptance_exact
          (@_basesort TNull (EmbedDB_Q1 db))
          (@_instance TNull (EmbedDB_Q1 db)) sched).
      * apply q1_group_select_list_exact; assumption.
Qed.

(** GroupBagOk under per-bucket labels + Safe overflow packaging.
    Nonempty of each bucket follows from [q1_group_terms] nonempty. *)
Lemma Q1_GroupBagOk_holds :
  forall db inner_proj,
    (forall group,
      In group (query_make_groups nil inner_proj q1_group_terms) ->
      Forall q1_group_row_labels_ok group /\
      q1_group_bucket_safe group) ->
    Q1_GroupBagOk db inner_proj
      (map (fun group => project_row q1_group_select
        (q1_group_select_values group))
        (query_make_groups nil inner_proj q1_group_terms)).
Proof.
  intros db inner_proj Hbucket.
  apply Q1_GroupBagOk_of_exact_map_having_true with
    (group_terms := q1_group_terms) (values := q1_group_select_values).
  - exact q1_group_keys_terms.
  - intros sched; split.
    + apply q1_group_dot_keys_runtime_none.
    + intros groups g Hg.
      destruct (Hbucket g Hg) as [Hlab Hsafe].
      assert (Hne : g <> nil).
      { eapply query_make_groups_member_nonempty;
          [exact q1_group_terms_nonempty|exact Hg]. }
      apply q1_group_filter_map_exact; assumption.
Qed.

(** * AuxCorrect leaf-chain assembly (Pre-derived rows + residual hyps)

    Labels discharge from [project_row] schemas.  Full [q1_group_bucket_safe]
    (COUNT + SUM×4 + AVG×3) discharges from Pre.  Asserted
    [FormalSQLAuxCorrect_Q1] still needs SpecView↔[q1_final_rows_of]
    list equality as an explicit hyp. *)

Definition q1_filtered_aux (db : list Q1AuxSpec.Row) (delta : Z)
  : list Q1AuxSpec.Row :=
  List.filter
    (fun r => Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta)) db.

Definition q1_inner_proj_of (db : list Q1AuxSpec.Row) (delta : Z)
  : list (tuple TNull) :=
  map (fun r => project_row q1_inner_select (q1_inner_values r))
    (q1_filtered_aux db delta).

Definition q1_group_rows_of (db : list Q1AuxSpec.Row) (delta : Z)
  : list (tuple TNull) :=
  map (fun group => project_row q1_group_select
    (q1_group_select_values group))
    (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms).

Definition q1_ordered_rows_of (db : list Q1AuxSpec.Row) (delta : Z)
  : list (tuple TNull) :=
  @sort_ordered_rows TNull value_is_null q1_order_keys
    (q1_group_rows_of db delta).

Definition q1_final_rows_of (db : list Q1AuxSpec.Row) (delta : Z)
  : list (tuple TNull) :=
  map (fun row => project_row q1_outer_select (q1_outer_dot_values row))
    (q1_ordered_rows_of db delta).

Lemma q1_project_row_labels :
  forall select_list values,
    labels TNull (@project_row TNull relname select_list values) =S=
    Fset.mk_set (A TNull) (scalar_select_outputs select_list).
Proof.
  intros select_list values.
  unfold project_row, scalar_select_outputs.
  apply labels_mk_tuple.
Qed.

Lemma q1_attr_inS_project_row :
  forall select_list values attr,
    attr inS Fset.mk_set (A TNull) (scalar_select_outputs select_list) ->
    attr inS labels TNull (@project_row TNull relname select_list values).
Proof.
  intros select_list values attr Hin.
  rewrite (Fset.mem_eq_2 _ _ _ (q1_project_row_labels select_list values)).
  exact Hin.
Qed.

Lemma q1_inner_select_attr_inS_rf :
  q1_attr_rf inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_rf, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrString "l_returnflag" (StringChar 1) inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_inner_select_attr_inS_ls :
  q1_attr_ls inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_ls, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrString "l_linestatus" (StringChar 1) inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_inner_select_attr_inS_qty :
  q1_attr_qty inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_qty, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrDecimal "l_quantity" 15 2 inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_inner_select_attr_inS_price :
  q1_attr_price inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_price, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrDecimal "l_extendedprice" 15 2 inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_inner_select_attr_inS_disc :
  q1_attr_disc inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_disc, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrDecimal "l_discount" 15 2 inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_inner_select_attr_inS_f4 :
  q1_attr_f4 inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_f4, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrNumeric "$f4" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_inner_select_attr_inS_f5 :
  q1_attr_f5 inS Fset.mk_set (A TNull) (scalar_select_outputs q1_inner_select).
Proof.
  unfold q1_attr_f5, q1_inner_select, Queries.scalar_select_list_2,
    scalar_select_outputs.
  change (AttrNumeric "$f5" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_2) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_row_labels_ok_of_inner_values :
  forall r,
    q1_group_row_labels_ok
      (project_row q1_inner_select (q1_inner_values r)).
Proof.
  intro r.
  unfold q1_group_row_labels_ok.
  refine (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ _)))))).
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_rf.
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_ls.
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_qty.
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_price.
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_disc.
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_f4.
  - apply q1_attr_inS_project_row; exact q1_inner_select_attr_inS_f5.
Qed.

Lemma q1_group_select_attr_inS_rf :
  AttrString "l_returnflag" (StringChar 1)
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrString "l_returnflag" (StringChar 1) inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_ls :
  AttrString "l_linestatus" (StringChar 1)
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrString "l_linestatus" (StringChar 1) inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_sum_qty :
  AttrNumeric "sum_qty"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "sum_qty" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_sum_base :
  AttrNumeric "sum_base_price"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "sum_base_price" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_sum_disc :
  AttrNumeric "sum_disc_price"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "sum_disc_price" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_sum_charge :
  AttrNumeric "sum_charge"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "sum_charge" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_avg_qty :
  AttrNumeric "avg_qty"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "avg_qty" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_avg_price :
  AttrNumeric "avg_price"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "avg_price" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_avg_disc :
  AttrNumeric "avg_disc"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrNumeric "avg_disc" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_group_select_attr_inS_count :
  AttrInt64 "count_order"
    inS Fset.mk_set (A TNull) (scalar_select_outputs q1_group_select).
Proof.
  unfold q1_group_select, Queries.scalar_select_list_1, scalar_select_outputs.
  change (AttrInt64 "count_order" inS?
            Fset.mk_set (A TNull)
              (map snd Queries.scalar_select_list_1) = true).
  vm_compute; reflexivity.
Qed.

Lemma q1_outer_row_labels_ok_of_group_values :
  forall group,
    q1_outer_row_labels_ok
      (project_row q1_group_select (q1_group_select_values group)).
Proof.
  intro group.
  unfold q1_outer_row_labels_ok.
  refine (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ _))))))))).
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_rf.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_ls.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_sum_qty.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_sum_base.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_sum_disc.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_sum_charge.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_avg_qty.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_avg_price.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_avg_disc.
  - apply q1_attr_inS_project_row; exact q1_group_select_attr_inS_count.
Qed.

Lemma Forall_RowOk_q1_filtered_aux :
  forall db delta,
    Forall RowOk_Q1 db ->
    Forall RowOk_Q1 (q1_filtered_aux db delta).
Proof.
  intros db delta Hok.
  unfold q1_filtered_aux.
  induction db as [|r rs IH]; [constructor|].
  inversion Hok; subst.
  cbn [List.filter].
  destruct (Z.leb (Q1AuxSpec.shipdate r) (EmbedParams_Q1 delta));
    [constructor; [assumption|apply IH; assumption]|apply IH; assumption].
Qed.

Lemma q1_inner_proj_Forall_group_labels :
  forall db delta,
    Forall q1_group_row_labels_ok (q1_inner_proj_of db delta).
Proof.
  intros db delta.
  unfold q1_inner_proj_of.
  induction (q1_filtered_aux db delta) as [|r rs IH];
    [constructor|constructor; [apply q1_group_row_labels_ok_of_inner_values|exact IH]].
Qed.

Lemma q1_group_buckets_labels_ok :
  forall db delta group,
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    Forall q1_group_row_labels_ok group.
Proof.
  intros db delta group Hin.
  apply Forall_forall; intros row Hrow.
  eapply Forall_forall.
  - exact (q1_inner_proj_Forall_group_labels db delta).
  - eapply query_make_groups_member_in; eassumption.
Qed.

Lemma q1_group_rows_outer_labels :
  forall db delta row,
    In row (q1_group_rows_of db delta) ->
    q1_outer_row_labels_ok row.
Proof.
  intros db delta row Hin.
  unfold q1_group_rows_of in Hin.
  apply in_map_iff in Hin.
  destruct Hin as [group [Heq Hgin]].
  subst row.
  apply q1_outer_row_labels_ok_of_group_values.
Qed.

Lemma q1_ordered_rows_outer_labels :
  forall db delta row,
    In row (q1_ordered_rows_of db delta) ->
    q1_outer_row_labels_ok row.
Proof.
  intros db delta row Hin.
  apply q1_group_rows_outer_labels with (db := db) (delta := delta).
  eapply Permutation_in; [|exact Hin].
  apply Permutation_sym.
  unfold q1_ordered_rows_of.
  apply sort_ordered_rows_permutation.
Qed.

Lemma int64_result_runtime_error_none_of_bounds :
  forall z,
    int64_min <= z <= int64_max ->
    NullValues.int64_result_runtime_error z = None.
Proof.
  intros z [Hlo Hhi].
  unfold NullValues.int64_result_runtime_error, int64_checked.
  destruct (Z.leb_spec0 int64_min z) as [Hlo'|Hlo']; [|lia].
  destruct (Z.leb_spec0 z int64_max) as [Hhi'|Hhi']; [|lia].
  reflexivity.
Qed.

Lemma q1_group_bucket_count_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.int64_result_runtime_error (Z.of_nat (List.length group)) = None.
Proof.
  intros db delta group Hpre Hin.
  destruct Hpre as [_ [_ [_ Hcard]]].
  apply int64_result_runtime_error_none_of_bounds.
  split.
  - unfold int64_min; lia.
  - assert (Hlen :
      (List.length group <= List.length (q1_inner_proj_of db delta))%nat).
    { eapply query_make_groups_member_length_le; exact Hin. }
    unfold q1_inner_proj_of in Hlen.
    rewrite length_map in Hlen.
    assert (Hfilt :
      (List.length (q1_filtered_aux db delta) <= List.length db)%nat).
    { unfold q1_filtered_aux. apply List.filter_length_le. }
    apply Z.le_trans with (m := Z.of_nat (List.length db)); [|exact Hcard].
    apply Nat2Z.inj_le.
    lia.
Qed.

(** * Inner-project dots (schema positions of [q1_inner_values]) *)

Lemma q1_inner_project_dot_rf :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_rf =
    embed_flag_char (Q1AuxSpec.rf r).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_rf.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_project_dot_ls :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_ls =
    embed_flag_char (Q1AuxSpec.ls r).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_ls.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_project_dot_qty :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_qty =
    embed_decimal2 (Q1AuxSpec.qty r).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_qty.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_project_dot_price :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_price =
    embed_decimal2 (Q1AuxSpec.price r).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_price.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_project_dot_disc :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_disc =
    embed_decimal2 (Q1AuxSpec.disc r).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_disc.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_project_dot_f4 :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_f4 =
    NullValues.Value_numeric (Some (q1_f4_num r)).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_f4, q1_f4_num.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_project_dot_f5 :
  forall r,
    dot TNull (project_row q1_inner_select (q1_inner_values r)) q1_attr_f5 =
    NullValues.Value_numeric (Some (q1_f5_num r)).
Proof.
  intro r.
  unfold project_row, scalar_select_outputs, q1_inner_select,
    Queries.scalar_select_list_2, q1_inner_values, q1_attr_f5, q1_f5_num.
  cbn [map snd combine].
  rewrite dot_mk_tuple.
  match goal with
  | |- context [if ?b then _ else _] =>
      assert (Hb : b = true) by (vm_compute; reflexivity);
      rewrite Hb; clear Hb
  end.
  cbn; reflexivity.
Qed.

Lemma q1_inner_proj_in_inv :
  forall db delta row,
    In row (q1_inner_proj_of db delta) ->
    exists r,
      In r (q1_filtered_aux db delta) /\
      row = project_row q1_inner_select (q1_inner_values r).
Proof.
  intros db delta row Hin.
  unfold q1_inner_proj_of in Hin.
  apply in_map_iff in Hin.
  destruct Hin as [r [Heq Hinr]].
  exists r; split; [exact Hinr|symmetry; exact Heq].
Qed.


Lemma q1_filtered_aux_RowOk :
  forall db delta r,
    Preconditions_Q1 db delta ->
    In r (q1_filtered_aux db delta) ->
    RowOk_Q1 r.
Proof.
  intros db delta r Hpre Hin.
  destruct Hpre as [_ [_ [Hrows _]]].
  unfold q1_filtered_aux in Hin.
  apply filter_In in Hin; destruct Hin as [Hin _].
  eapply Forall_forall; eassumption.
Qed.

Lemma q1_group_member_aux_row :
  forall db delta group row,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    In row group ->
    exists r,
      RowOk_Q1 r /\
      row = project_row q1_inner_select (q1_inner_values r).
Proof.
  intros db delta group row Hpre Hgin Hrow.
  assert (Hin_proj : In row (q1_inner_proj_of db delta)).
  { eapply query_make_groups_member_in; eassumption. }
  destruct (q1_inner_proj_in_inv db delta row Hin_proj) as [r [Hinr Heq]].
  exists r; split; [|exact Heq].
  eapply q1_filtered_aux_RowOk; eassumption.
Qed.

(** Embed DECIMAL(15,2) conforms under coefficient digit bound. *)
Lemma embed_decimal2_conforms_15_2 :
  forall name c,
    Z.abs c < 10 ^ 15 ->
    value_conforms_attribute (AttrDecimal name 15 2) (embed_decimal2 c).
Proof.
  intros name c Hc.
  unfold value_conforms_attribute, embed_decimal2, AttrDecimal.
  apply numeric_cast_typmod_of_scaled_15_2; exact Hc.
Qed.

Lemma RowOk_qty_conforms :
  forall r, RowOk_Q1 r ->
    value_conforms_attribute (AttrDecimal "l_quantity" 15 2)
      (embed_decimal2 (Q1AuxSpec.qty r)).
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [Hq _]]]].
  apply embed_decimal2_conforms_15_2.
  rewrite Z.abs_eq by lia.
  assert (5000 < Z.pow 10 15).
  { change (Z.pow 10 15) with 1000000000000000. lia. }
  lia.
Qed.

Lemma RowOk_price_conforms :
  forall r, RowOk_Q1 r ->
    value_conforms_attribute (AttrDecimal "l_extendedprice" 15 2)
      (embed_decimal2 (Q1AuxSpec.price r)).
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [_ [Hp _]]]]].
  apply embed_decimal2_conforms_15_2.
  rewrite Z.abs_eq by lia.
  assert (1000000000000 < Z.pow 10 15).
  { change (Z.pow 10 15) with 1000000000000000. lia. }
  lia.
Qed.

Lemma RowOk_disc_conforms :
  forall r, RowOk_Q1 r ->
    value_conforms_attribute (AttrDecimal "l_discount" 15 2)
      (embed_decimal2 (Q1AuxSpec.disc r)).
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [_ [_ [Hd _]]]]]].
  apply embed_decimal2_conforms_15_2.
  rewrite Z.abs_eq by lia.
  assert (10 < Z.pow 10 15).
  { change (Z.pow 10 15) with 1000000000000000. lia. }
  lia.
Qed.

(** Accumulator bound for DECIMAL(15,2) SUM under Pre cardinality. *)
Definition q1_decimal15_sum_accumulator (max_count : Z) : Z :=
  max_count * Z.pow 10 15.

Lemma q1_scaled_sum_accumulator_fits :
  forall scale max_count c,
    0 <= scale <= postgres_numeric_max_fractional_digits ->
    0 <= max_count <= int64_max ->
    Z.abs c <= q1_decimal15_sum_accumulator max_count ->
    numeric_runtime_fits_bool (numeric_of_scaled c scale) = true.
Proof.
  intros scale max_count c Hs [Hmc0 Hmc] Habs.
  unfold q1_decimal15_sum_accumulator in Habs.
  apply numeric_of_scaled_runtime_fits_of_digit_bound.
  - exact Hs.
  - assert (Habs' : Z.abs c <= int64_max * Z.pow 10 15).
    { eapply Z.le_trans; [exact Habs|].
      apply Z.mul_le_mono_nonneg_r; [apply Z.pow_nonneg; lia|exact Hmc]. }
    assert (Hle : Z.abs c <= Z.pow 10 35).
    { eapply Z.le_trans; [exact Habs'|].
      change (Z.pow 10 15) with 1000000000000000.
      change (Z.pow 10 35) with
        100000000000000000000000000000000000.
      unfold int64_max; lia. }
    pose proof (numeric_digit_count_le_of_le_pow c 35 ltac:(lia) Hle) as Hd.
    eapply Z.le_trans; [exact Hd|].
    unfold postgres_numeric_max_integer_digits; lia.
Qed.

Lemma q1_decimal15_sum_accumulator_fits :
  forall max_count c,
    0 <= max_count <= int64_max ->
    Z.abs c <= q1_decimal15_sum_accumulator max_count ->
    numeric_runtime_fits_bool (numeric_of_scaled c 2) = true.
Proof.
  intros max_count c Hmc Habs.
  apply (q1_scaled_sum_accumulator_fits 2 max_count c);
    [unfold postgres_numeric_max_fractional_digits; lia|exact Hmc|exact Habs].
Qed.

Lemma q1_group_length_le_db :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    (List.length group <= List.length db)%nat.
Proof.
  intros db delta group Hpre Hin.
  assert (Hlen :
    (List.length group <= List.length (q1_inner_proj_of db delta))%nat).
  { eapply query_make_groups_member_length_le; exact Hin. }
  unfold q1_inner_proj_of in Hlen.
  rewrite length_map in Hlen.
  assert (Hfilt :
    (List.length (q1_filtered_aux db delta) <= List.length db)%nat).
  { unfold q1_filtered_aux. apply List.filter_length_le. }
  lia.
Qed.

Lemma Forall_map_conforms_qty :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    Forall
      (value_conforms_attribute (AttrDecimal "l_quantity" 15 2))
      (map (fun row => dot TNull row q1_attr_qty) group).
Proof.
  intros db delta group Hpre Hin.
  apply Forall_forall; intros v Hv.
  apply in_map_iff in Hv.
  destruct Hv as [row [Heqv Hinrow]].
  subst v.
  destruct (q1_group_member_aux_row db delta group row Hpre Hin Hinrow)
    as [r [Hok Heq]].
  rewrite Heq, q1_inner_project_dot_qty.
  apply RowOk_qty_conforms; exact Hok.
Qed.

Lemma Forall_map_conforms_price :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    Forall
      (value_conforms_attribute (AttrDecimal "l_extendedprice" 15 2))
      (map (fun row => dot TNull row q1_attr_price) group).
Proof.
  intros db delta group Hpre Hin.
  apply Forall_forall; intros v Hv.
  apply in_map_iff in Hv.
  destruct Hv as [row [Heqv Hinrow]].
  subst v.
  destruct (q1_group_member_aux_row db delta group row Hpre Hin Hinrow)
    as [r [Hok Heq]].
  rewrite Heq, q1_inner_project_dot_price.
  apply RowOk_price_conforms; exact Hok.
Qed.

Lemma Forall_map_conforms_disc :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    Forall
      (value_conforms_attribute (AttrDecimal "l_discount" 15 2))
      (map (fun row => dot TNull row q1_attr_disc) group).
Proof.
  intros db delta group Hpre Hin.
  apply Forall_forall; intros v Hv.
  apply in_map_iff in Hv.
  destruct Hv as [row [Heqv Hinrow]].
  subst v.
  destruct (q1_group_member_aux_row db delta group row Hpre Hin Hinrow)
    as [r [Hok Heq]].
  rewrite Heq, q1_inner_project_dot_disc.
  apply RowOk_disc_conforms; exact Hok.
Qed.

Lemma q1_group_sum_qty_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_qty) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_qty) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
  - eapply Forall_map_conforms_qty; [exact Hpre|exact Hin].
Qed.

Lemma q1_group_sum_price_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_price) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_price) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
  - eapply Forall_map_conforms_price; [exact Hpre|exact Hin].
Qed.

(** $f4/$f5 are AttrNumeric carriers equal to [numeric_of_scaled] at scales 4/6. *)
Lemma q1_f4_num_of_scaled :
  forall r,
    q1_f4_num r =
    numeric_of_scaled (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)) 4.
Proof.
  intro r.
  unfold q1_f4_num.
  rewrite numeric_one_minus_scaled2.
  rewrite numeric_mul_of_scaled_nonneg by lia.
  reflexivity.
Qed.

Lemma q1_f5_num_of_scaled :
  forall r,
    q1_f5_num r =
    numeric_of_scaled
      (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r) * (100 + Q1AuxSpec.tax r)) 6.
Proof.
  intro r.
  unfold q1_f5_num.
  rewrite q1_f4_num_of_scaled, numeric_one_plus_scaled2.
  rewrite numeric_mul_of_scaled_nonneg by lia.
  f_equal; ring.
Qed.

Lemma q1_f4_coeff_bound :
  forall r, RowOk_Q1 r ->
    Z.abs (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)) < Z.pow 10 15.
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [_ [Hp [Hd _]]]]]].
  rewrite Z.abs_eq by nia.
  assert (Hprod : Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
                    <= 1000000000000 * 100) by nia.
  eapply Z.le_lt_trans; [exact Hprod|].
  change (1000000000000 * 100) with 100000000000000.
  change (Z.pow 10 15) with 1000000000000000.
  lia.
Qed.

Lemma q1_f5_coeff_bound :
  forall r, RowOk_Q1 r ->
    Z.abs (Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r)
             * (100 + Q1AuxSpec.tax r)) < Z.pow 10 17.
Proof.
  intros r Hrow.
  destruct Hrow as [_ [_ [_ [_ [Hp [Hd Ht]]]]]].
  rewrite Z.abs_eq by nia.
  assert (Hprod :
    Q1AuxSpec.price r * (100 - Q1AuxSpec.disc r) * (100 + Q1AuxSpec.tax r)
      <= 1000000000000 * 100 * 108) by nia.
  eapply Z.le_lt_trans; [exact Hprod|].
  change (1000000000000 * 100 * 108) with 10800000000000000.
  change (Z.pow 10 17) with 100000000000000000.
  lia.
Qed.

Lemma q1_group_sum_f4_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_f4) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_f4) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
    destruct (q1_group_member_aux_row db delta group row Hpre Hin Hinrow)
      as [r [Hok Heq]].
    rewrite Heq, q1_inner_project_dot_f4, q1_f4_num_of_scaled.
    apply fixed_decimal_value_shape_of_scaled; [lia|].
    apply q1_f4_coeff_bound; exact Hok.
Qed.

Lemma q1_group_sum_f5_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.sum_numeric_runtime_error
      (map (fun row => dot TNull row q1_attr_f5) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  set (acc := max_count * Z.pow 10 17).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_f5) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
    destruct (q1_group_member_aux_row db delta group row Hpre Hin Hinrow)
      as [r [Hok Heq]].
    rewrite Heq, q1_inner_project_dot_f5, q1_f5_num_of_scaled.
    apply fixed_decimal_value_shape_of_scaled; [lia|].
    apply q1_f5_coeff_bound; exact Hok.
Qed.

Lemma q1_group_avg_qty_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.avg_numeric_fixed_runtime_error 15 2
      (map (fun row => dot TNull row q1_attr_qty) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_qty) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
  - eapply Forall_map_conforms_qty; [exact Hpre|exact Hin].
Qed.

Lemma q1_group_avg_price_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.avg_numeric_fixed_runtime_error 15 2
      (map (fun row => dot TNull row q1_attr_price) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_price) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
  - eapply Forall_map_conforms_price; [exact Hpre|exact Hin].
Qed.

Lemma q1_group_avg_disc_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    NullValues.avg_numeric_fixed_runtime_error 15 2
      (map (fun row => dot TNull row q1_attr_disc) group) = None.
Proof.
  intros db delta group Hpre Hin.
  set (max_count := Z.of_nat (List.length db)).
  assert (Hlen : Z.of_nat (List.length
    (map (fun row => dot TNull row q1_attr_disc) group)) <= max_count).
  { rewrite length_map. apply Nat2Z.inj_le.
    eapply q1_group_length_le_db; [exact Hpre|exact Hin]. }
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
  - eapply Forall_map_conforms_disc; [exact Hpre|exact Hin].
Qed.

(** Full per-bucket overflow packaging from Pre (COUNT + SUM×4 + AVG×3). *)
Lemma q1_group_bucket_safe_of_pre :
  forall db delta group,
    Preconditions_Q1 db delta ->
    In group (query_make_groups nil (q1_inner_proj_of db delta) q1_group_terms) ->
    q1_group_bucket_safe group.
Proof.
  intros db delta group Hpre Hin.
  refine (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ (conj _ _))))))).
  - eapply q1_group_sum_qty_safe_of_pre; eassumption.
  - eapply q1_group_sum_price_safe_of_pre; eassumption.
  - eapply q1_group_sum_f4_safe_of_pre; eassumption.
  - eapply q1_group_sum_f5_safe_of_pre; eassumption.
  - eapply q1_group_avg_qty_safe_of_pre; eassumption.
  - eapply q1_group_avg_price_safe_of_pre; eassumption.
  - eapply q1_group_avg_disc_safe_of_pre; eassumption.
  - eapply q1_group_bucket_count_safe_of_pre; eassumption.
Qed.

(** Leaf-chain AuxCorrect under SpecView list equality.
    All of [q1_group_bucket_safe] discharges from Pre via digit-budget facades.
    The top-level [FormalSQLAuxCorrect_Q1] is restated in [Q1TCBAuxCorrect.v]
    under [NumericEmbedFaithful] + [Q1_GroupsCorrespond] (discharges this hyp). *)
Lemma FormalSQLAuxCorrect_Q1_of_holds :
  forall db delta,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    q1_final_rows_of db delta =
      EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta)) ->
    FormalSQL_exec (EmbedDB_Q1 db) delta
      (EmbedResult_Q1 (Q1AuxSpec.AuxAnswerSpec db (EmbedParams_Q1 delta))).
Proof.
  intros db delta Hpre Hsafe Hfinal.
  set (filtered := map EmbedRow_Q1 (q1_filtered_aux db delta)).
  set (inner_proj := q1_inner_proj_of db delta).
  set (group_rows := q1_group_rows_of db delta).
  set (ordered_rows := q1_ordered_rows_of db delta).
  eapply FormalSQLAuxCorrect_Q1_of_leaves
    with (filtered := filtered) (inner_proj := inner_proj)
         (group_rows := group_rows) (ordered_rows := ordered_rows);
    try assumption.
  - unfold filtered, q1_filtered_aux.
    apply Q1_FilterRowsOk_holds; exact Hpre.
  - unfold filtered, inner_proj, q1_inner_proj_of, q1_filtered_aux.
    apply Q1_InnerProjectOk_holds.
    apply Forall_RowOk_q1_filtered_aux.
    destruct Hpre as [_ [_ [Hrows _]]]; exact Hrows.
  - unfold group_rows, q1_group_rows_of, inner_proj.
    apply Q1_GroupBagOk_holds.
    intros group Hin; split.
    + exact (q1_group_buckets_labels_ok db delta group Hin).
    + exact (q1_group_bucket_safe_of_pre db delta group Hpre Hin).
  - unfold ordered_rows, group_rows, q1_ordered_rows_of, q1_group_rows_of.
    apply Q1_OrderOk_canonical.
  - rewrite <- Hfinal.
    unfold q1_final_rows_of, ordered_rows.
    apply Q1_OuterProjectOk_holds.
    intros row Hin; exact (q1_ordered_rows_outer_labels db delta row Hin).
Qed.

(** Top-level nonempty AuxCorrect lives in [Q1TCBAuxCorrect.v]
    (Pre + Safe + NumericEmbedFaithful + Q1_GroupsCorrespond). *)

