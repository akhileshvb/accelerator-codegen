(* Q12JoinSpine — instantiate QueryJoin2SpineFacade for TPC-H Q12.

   Proves FormalSQL_exec_Q12(EmbedDB, Embed(EvalSpec)) under Pre_Q12,
   using the shared 2-table join spine + Q12SemanticBridge leaf facts.
*)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  SqlListFacts FiniteBag FiniteCollection FiniteSet FTuples Bool3 OrderedSet Env
  FlatData Projection.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts
  FormalSQL.AggregateRuntimeFacts FormalSQL.VerificationConditions
  FormalSQL.GroupedFilterOutcomeFacts FormalSQL.QueryCardinality.
From FormalSQLLib Require Import
  QueryJoin2SpineFacade OrderRowsFacade GroupBagExactFacade
  FilterAcceptanceFacade GroupsExactFacade ProjectExactFacade
  NumericCallExactFacade TemporalPredAcceptanceFacade.
From LogosGenerated Require Import Schema Queries.
From FormalSQLResultSpec Require Import
  QueryResultSpecQ12 Q12EvalSpec Q12SemanticBridge.
From Stdlib Require Import String Ascii List ZArith.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q12Catalog.
Require Import Q12NormalizeSound.
Require Import Q12EmbedDots.
Require Import Q12WhereAcceptance.
Require Import Q12CaseAcceptance.
Require Import Q12GroupAcceptance.

Import Q12EvalSpec.

(** * Boolean schedule + null test (match NormalizeSound) *)

Definition q12_boolean_schedule : boolean_site -> boolean_evaluation_order :=
  fun _ => BooleanLeftFirst.

Definition q12_value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

(** * AST: source_query_expr ≡ query_spine_join2 *)

Definition q12_orders_outputs : list (attribute TNull) :=
  [AttrInt32 "o_orderkey"; AttrInt32 "o_custkey";
   AttrString "o_orderstatus" (StringChar 1); AttrDecimal "o_totalprice" 15 2;
   AttrDate "o_orderdate"; AttrString "o_orderpriority" (StringChar 15);
   AttrString "o_clerk" (StringChar 15); AttrInt32 "o_shippriority";
   AttrString "o_comment" (StringVarcharN 79)].

Definition q12_lineitem_outputs : list (attribute TNull) :=
  [AttrInt32 "l_orderkey"; AttrInt32 "l_partkey"; AttrInt32 "l_suppkey";
   AttrInt32 "l_linenumber"; AttrDecimal "l_quantity" 15 2;
   AttrDecimal "l_extendedprice" 15 2; AttrDecimal "l_discount" 15 2;
   AttrDecimal "l_tax" 15 2; AttrString "l_returnflag" (StringChar 1);
   AttrString "l_linestatus" (StringChar 1); AttrDate "l_shipdate";
   AttrDate "l_commitdate"; AttrDate "l_receiptdate";
   AttrString "l_shipinstruct" (StringChar 25);
   AttrString "l_shipmode" (StringChar 10);
   AttrString "l_comment" (StringVarcharN 44)].

Definition q12_group_keys : list (scalar_expr TNull relname ScalarResultValue) :=
  [@SExpr_Leaf TNull relname type_string (DotString "l_shipmode" (StringChar 10))].

Definition q12_order_keys : list (sort_key TNull) :=
  [SortAscNullsLast (AttrString "l_shipmode" (StringChar 10))].

Definition q12_spine_expr : query_expr TNull relname :=
  @query_spine_join2 TNull relname
    q12_orders_outputs q12_lineitem_outputs
    (Rel "orders") (Rel "lineitem")
    Queries.scalar_select_list_3 Queries.scalar_select_list_4
    Queries.scalar_select_list_2 Queries.scalar_select_list_1
    Queries.scalar_select_list_0
    Queries.scalar_expr_predicate_0 Queries.scalar_expr_predicate_1
    Queries.scalar_expr_predicate_0
    q12_group_keys q12_order_keys.

Lemma source_query_expr_eq_spine :
  Queries.source_query_expr = q12_spine_expr.
Proof. reflexivity. Qed.

(** * Empty EmbedDB table observations *)

Lemma embed_nil_instance_orders :
  @_instance TNull (EmbedDB_Q12 nil nil) (Rel "orders")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. reflexivity. Qed.

Lemma embed_nil_instance_lineitem :
  @_instance TNull (EmbedDB_Q12 nil nil) (Rel "lineitem")
  = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof. reflexivity. Qed.

Lemma embed_nil_table_same_rows_orders :
  forall outputs,
    query_same_rows_as_bag (T:=TNull) nil
      (@query_table_bag TNull relname
         (@_basesort TNull (EmbedDB_Q12 nil nil))
         (@_instance TNull (EmbedDB_Q12 nil nil))
         outputs (Rel "orders")).
Proof.
  intro outputs.
  unfold query_same_rows_as_bag.
  rewrite Febag.nb_occ_equal; intro x.
  unfold query_table_bag.
  rewrite embed_nil_instance_orders.
  destruct (@query_outputs_sort TNull outputs =S?=
            @_basesort TNull (EmbedDB_Q12 nil nil) (Rel "orders"));
    rewrite ?Febag.nb_occ_mk_bag, ?Febag.nb_occ_empty; cbn; reflexivity.
Qed.

Lemma embed_nil_table_same_rows_lineitem :
  forall outputs,
    query_same_rows_as_bag (T:=TNull) nil
      (@query_table_bag TNull relname
         (@_basesort TNull (EmbedDB_Q12 nil nil))
         (@_instance TNull (EmbedDB_Q12 nil nil))
         outputs (Rel "lineitem")).
Proof.
  intro outputs.
  unfold query_same_rows_as_bag.
  rewrite Febag.nb_occ_equal; intro x.
  unfold query_table_bag.
  rewrite embed_nil_instance_lineitem.
  destruct (@query_outputs_sort TNull outputs =S?=
            @_basesort TNull (EmbedDB_Q12 nil nil) (Rel "lineitem"));
    rewrite ?Febag.nb_occ_mk_bag, ?Febag.nb_occ_empty; cbn; reflexivity.
Qed.

Lemma q12_empty_group_bag :
  forall sched,
    @eval_group_bag_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q12 nil nil))
      (@_instance TNull (EmbedDB_Q12 nil nil))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil Queries.scalar_select_list_1 q12_group_keys
      Queries.scalar_expr_predicate_0
      (query_rows_bag (@nil (tuple TNull)))
      (SqlSuccess (query_rows_bag nil)).
Proof.
  intros sched.
  eapply EGroupBag_Success.
  - reflexivity.
  - apply Febag.equal_refl.
  - reflexivity.
  - rewrite query_make_groups_empty_shape; cbn; apply EGroups_Nil.
  - apply Febag.equal_refl.
Qed.

Lemma q12_empty_order_rows :
  @order_by_rows TNull q12_value_is_null q12_order_keys nil nil.
Proof. split; [apply Febag.equal_refl | exact I]. Qed.

(** Empty DB: FormalSQL succeeds with nil (= Embed(EvalSpec nil nil)). *)
Lemma FormalSQL_Q12_empty_db_success_nil :
  FormalSQL_exec_Q12 (EmbedDB_Q12 nil nil) nil.
Proof.
  unfold FormalSQL_exec_Q12.
  rewrite source_query_expr_eq_spine.
  exists q12_boolean_schedule.
  eapply (@query_spine_join2_empty_success TNull relname
    (@_basesort TNull (EmbedDB_Q12 nil nil))
    (@_instance TNull (EmbedDB_Q12 nil nil))
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    q12_value_is_null
    q12_boolean_schedule).
  - apply embed_nil_table_same_rows_orders.
  - apply embed_nil_table_same_rows_lineitem.
  - apply q12_empty_group_bag.
  - apply q12_empty_order_rows.
Qed.

Lemma FormalSQLAuxCorrect_Q12_nil :
  Preconditions_Q12 nil nil ->
  FormalSQL_exec_Q12 (EmbedDB_Q12 nil nil)
    (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog nil nil)).
Proof.
  intros Hpre.
  rewrite NormalizeSound_Q12_nil_eval.
  unfold EmbedResult_Q12; cbn.
  apply FormalSQL_Q12_empty_db_success_nil.
Qed.

(** * Leaf Ok contracts (join2 spine obligations)

    Query-specific work discharges these; the facade composes them into
    [FormalSQL_exec].  A future 2-table query supplies the same shape. *)

Definition Q12_TableOrdersOk (orders : list OrderRow) (lines : list LineRow)
    (order_rows : list (tuple TNull)) : Prop :=
  order_rows = map EmbedOrderRow orders /\
  query_same_rows_as_bag order_rows
    (@query_table_bag TNull relname
       (@_basesort TNull (EmbedDB_Q12 orders lines))
       (@_instance TNull (EmbedDB_Q12 orders lines))
       q12_orders_outputs (Rel "orders")).

Definition Q12_TableLinesOk (orders : list OrderRow) (lines : list LineRow)
    (line_rows : list (tuple TNull)) : Prop :=
  line_rows = map EmbedLineRow lines /\
  query_same_rows_as_bag line_rows
    (@query_table_bag TNull relname
       (@_basesort TNull (EmbedDB_Q12 orders lines))
       (@_instance TNull (EmbedDB_Q12 orders lines))
       q12_lineitem_outputs (Rel "lineitem")).

Definition Q12_ProjectTableOk
    (D : db_state) (sel : list (scalar_expr TNull relname ScalarResultValue *
                                attribute TNull))
    (input output : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil sel input (SqlSuccess output).

Definition Q12_JoinBagOk
    (proj_o proj_l join_rows : list (tuple TNull)) : Prop :=
  query_same_rows_as_bag join_rows
    (query_cross_join_bag
       (query_rows_bag proj_o) (query_rows_bag proj_l)).

Definition Q12_FilterOk
    (D : db_state)
    (formula : scalar_expr TNull relname ScalarResultBoolean)
    (input output : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil formula input (SqlSuccess output).

Definition Q12_GroupBagOk
    (D : db_state)
    (pre_group group_rows : list (tuple TNull)) : Prop :=
  forall sched,
    exists group_bag,
      @eval_group_bag_outcome TNull relname
        (@_basesort TNull D) (@_instance TNull D)
        unknown3 NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error q12_value_is_null sched
        nil Queries.scalar_select_list_1 q12_group_keys
        Queries.scalar_expr_predicate_0
        (query_rows_bag pre_group) (SqlSuccess group_bag) /\
      query_same_rows_as_bag group_rows group_bag.

Definition Q12_OrderOk
    (group_rows ordered_rows : list (tuple TNull)) : Prop :=
  @order_by_rows TNull q12_value_is_null q12_order_keys group_rows ordered_rows.

Definition Q12_OuterProjectOk
    (D : db_state)
    (ordered_rows final_rows : list (tuple TNull)) : Prop :=
  forall sched,
    @eval_project_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil Queries.scalar_select_list_0 ordered_rows (SqlSuccess final_rows).

(** Facade instantiation: leaves ⇒ FormalSQL_exec. Not TCB. *)
Lemma FormalSQLAuxCorrect_Q12_of_leaves :
  forall orders lines
    order_rows line_rows proj_o proj_l
    join_rows filt_true_rows where_rows pre_group_rows
    group_rows ordered_rows,
    Preconditions_Q12 orders lines ->
    Q12_TableOrdersOk orders lines order_rows ->
    Q12_TableLinesOk orders lines line_rows ->
    Q12_ProjectTableOk (EmbedDB_Q12 orders lines)
      Queries.scalar_select_list_3 order_rows proj_o ->
    Q12_ProjectTableOk (EmbedDB_Q12 orders lines)
      Queries.scalar_select_list_4 line_rows proj_l ->
    Q12_JoinBagOk proj_o proj_l join_rows ->
    Q12_FilterOk (EmbedDB_Q12 orders lines)
      Queries.scalar_expr_predicate_0 join_rows filt_true_rows ->
    Q12_FilterOk (EmbedDB_Q12 orders lines)
      Queries.scalar_expr_predicate_1 filt_true_rows where_rows ->
    Q12_ProjectTableOk (EmbedDB_Q12 orders lines)
      Queries.scalar_select_list_2 where_rows pre_group_rows ->
    Q12_GroupBagOk (EmbedDB_Q12 orders lines) pre_group_rows group_rows ->
    Q12_OrderOk group_rows ordered_rows ->
    Q12_OuterProjectOk (EmbedDB_Q12 orders lines) ordered_rows
      (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines)) ->
    FormalSQL_exec_Q12 (EmbedDB_Q12 orders lines)
      (EmbedResult_Q12 (EvalSpec_Q12 S_Q12_catalog orders lines)).
Proof.
  intros orders lines
    order_rows line_rows proj_o proj_l
    join_rows filt_true_rows where_rows pre_group_rows
    group_rows ordered_rows
    Hpre Hord Hlin Hpo Hpl Hjoin Hft Hw Hpreproj Hgroup Horder Houter.
  unfold FormalSQL_exec_Q12.
  rewrite source_query_expr_eq_spine.
  exists q12_boolean_schedule.
  destruct (Hgroup q12_boolean_schedule) as [group_bag [Hgeval Hgsame]].
  destruct Hord as [_ Hordersame].
  destruct Hlin as [_ Hlinesame].
  eapply (@query_spine_join2_success TNull relname
    (@_basesort TNull (EmbedDB_Q12 orders lines))
    (@_instance TNull (EmbedDB_Q12 orders lines))
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    q12_value_is_null
    q12_boolean_schedule).
  - exact Hordersame.
  - exact Hlinesame.
  - apply (Hpo q12_boolean_schedule).
  - apply (Hpl q12_boolean_schedule).
  - exact Hjoin.
  - apply (Hft q12_boolean_schedule).
  - apply (Hw q12_boolean_schedule).
  - apply (Hpreproj q12_boolean_schedule).
  - exact Hgeval.
  - exact Hgsame.
  - exact Horder.
  - apply (Houter q12_boolean_schedule).
Qed.

(** * Discharge table scans *)

Lemma embed_db_instance_orders :
  forall orders lines,
    @_instance TNull (EmbedDB_Q12 orders lines) (Rel "orders")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) (map EmbedOrderRow orders).
Proof. reflexivity. Qed.

Lemma embed_db_instance_lineitem :
  forall orders lines,
    @_instance TNull (EmbedDB_Q12 orders lines) (Rel "lineitem")
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) (map EmbedLineRow lines).
Proof. reflexivity. Qed.

Lemma Q12_TableOrdersOk_holds :
  forall orders lines,
    Q12_TableOrdersOk orders lines (map EmbedOrderRow orders).
Proof.
  intros orders lines; split; [reflexivity|].
  unfold query_same_rows_as_bag, query_rows_bag, query_table_bag.
  rewrite embed_db_instance_orders.
  assert (Hsort :
    @query_outputs_sort TNull q12_orders_outputs =S=
      @_basesort TNull (EmbedDB_Q12 orders lines) (Rel "orders")).
  {
    change (@query_outputs_sort TNull q12_orders_outputs =S=
      @_basesort TNull Schema.generated_schema (Rel "orders")).
    pose proof Queries.generated_table_sort_0 as Hs.
    (* Hs : basesort =S= mk_set outputs; query_outputs_sort = mk_set *)
    unfold query_outputs_sort in *.
    rewrite Fset.equal_spec in Hs |- *.
    intro a; symmetry; apply Hs.
  }
  rewrite Hsort.
  apply Febag.equal_refl.
Qed.

Lemma Q12_TableLinesOk_holds :
  forall orders lines,
    Q12_TableLinesOk orders lines (map EmbedLineRow lines).
Proof.
  intros orders lines; split; [reflexivity|].
  unfold query_same_rows_as_bag, query_rows_bag, query_table_bag.
  rewrite embed_db_instance_lineitem.
  assert (Hsort :
    @query_outputs_sort TNull q12_lineitem_outputs =S=
      @_basesort TNull (EmbedDB_Q12 orders lines) (Rel "lineitem")).
  {
    change (@query_outputs_sort TNull q12_lineitem_outputs =S=
      @_basesort TNull Schema.generated_schema (Rel "lineitem")).
    pose proof Queries.generated_table_sort_1 as Hs.
    unfold query_outputs_sort in *.
    rewrite Fset.equal_spec in Hs |- *.
    intro a; symmetry; apply Hs.
  }
  rewrite Hsort.
  apply Febag.equal_refl.
Qed.

(** Filter True is the identity on rows (acceptance always true). *)
(** Spine cross-join: raw EmbedJoin (identity table projects preserve dots). *)
Definition q12_spine_join_rows
    (orders : list OrderRow) (lines : list LineRow) : list (tuple TNull) :=
  q12_embed_join_rows orders lines.

Lemma q12_spine_join_rows_eq_embed :
  forall orders lines,
    q12_spine_join_rows orders lines = q12_embed_join_rows orders lines.
Proof. intros; reflexivity. Qed.

(** WHERE filter on spine cross-join (EmbedJoin rows). *)
Lemma Q12_FilterOk_where_holds :
  forall orders lines,
    (forall o l, In o orders -> In l lines -> q12_pair_domain_ok o l) ->
    Q12_FilterOk (EmbedDB_Q12 orders lines)
      Queries.scalar_expr_predicate_1
      (q12_spine_join_rows orders lines)
      (q12_where_rows orders lines).
Proof.
  intros orders lines Hdom sched.
  unfold Q12_FilterOk.
  rewrite q12_spine_join_rows_eq_embed.
  pose proof (q12_where_rows_dots_eq orders lines Hdom) as Heqout.
  pose proof (q12_filter_where_dots_success
                (EmbedDB_Q12 orders lines) orders lines Hdom sched) as Hfilt.
  rewrite Heqout in Hfilt; exact Hfilt.
Qed.

(** CASE project (select_list_2) on WHERE rows → pre-group rows. *)
Lemma Q12_ProjectTableOk_case_holds :
  forall orders lines,
    (forall o l, In o orders -> In l lines ->
       q12_priority_embed_ok (o_priority o)) ->
    Q12_ProjectTableOk (EmbedDB_Q12 orders lines)
      Queries.scalar_select_list_2
      (q12_where_rows orders lines)
      (q12_pre_group_rows orders lines).
Proof.
  intros orders lines Hpri sched.
  unfold Q12_ProjectTableOk.
  apply (q12_project_case_success (EmbedDB_Q12 orders lines) sched
           orders lines Hpri).
Qed.

(** * GroupBagOk scaffolding (GroupsExact having-true; SumInt32 leaf next) *)

Definition q12_group_terms : list (aggterm TNull) :=
  [DotString "l_shipmode" (StringChar 10)].

Lemma q12_group_keys_terms :
  scalar_group_key_terms q12_group_keys = Some q12_group_terms.
Proof.
  unfold q12_group_keys, q12_group_terms; reflexivity.
Qed.

Lemma q12_group_dot_keys_runtime_none :
  forall rows,
    @group_keys_runtime_error TNull
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error
      nil q12_group_terms rows = None.
Proof.
  induction rows as [|row rows IH]; [reflexivity|].
  cbn [group_keys_runtime_error first_runtime_error].
  unfold q12_group_terms.
  cbn [eval_aggterm_runtime_error eval_funterm_runtime_error].
  exact IH.
Qed.

Lemma Q12_GroupBagOk_of_exact_map_having_true :
  forall D pre_group group_terms values,
    scalar_group_key_terms q12_group_keys = Some group_terms ->
    (forall sched,
      @group_keys_runtime_error TNull
        NullValues.interp_scalar_operator_runtime_error
        NullValues.interp_aggregate_runtime_error
        nil group_terms pre_group = None /\
      let groups := query_make_groups nil pre_group group_terms in
      forall group,
        In group groups ->
        @group_filter_map_exact_at TNull relname
          (@_basesort TNull D) (@_instance TNull D)
          unknown3 NullValues.interp_scalar_operator_runtime_error
          NullValues.interp_aggregate_runtime_error q12_value_is_null sched
          nil Queries.scalar_select_list_1 group_terms
          Queries.scalar_expr_predicate_0
          (fun _ => true) values group) ->
    Q12_GroupBagOk D pre_group
      (map (fun group =>
        project_row Queries.scalar_select_list_1 (values group))
        (query_make_groups nil pre_group group_terms)).
Proof.
  intros D pre_group group_terms values Hterms Hexact sched.
  exists
    (query_rows_bag
      (map (fun group =>
        project_row Queries.scalar_select_list_1 (values group))
        (query_make_groups nil pre_group group_terms))).
  destruct (Hexact sched) as [Hkeys Hgroups].
  split.
  - eapply (@tnull_group_bag_success_of_exact_map_having_true
      (@_basesort TNull D) (@_instance TNull D) sched).
    + exact Hterms.
    + unfold query_same_rows_as_bag; apply Febag.equal_refl.
    + exact Hkeys.
    + intros group Hg.
      (* having is SExpr_True (= scalar_expr_predicate_0) *)
      unfold Queries.scalar_expr_predicate_0 in Hgroups.
      apply Hgroups; exact Hg.
  - apply Febag.equal_refl.
Qed.

Lemma Q12_GroupBagOk_holds :
  forall D pre_group,
    (forall group,
      In group (query_make_groups nil pre_group q12_group_terms) ->
      Forall q12_group_row_labels_ok group /\
      q12_group_bucket_safe group) ->
    Q12_GroupBagOk D pre_group
      (map (fun group =>
        project_row Queries.scalar_select_list_1
          (q12_group_select_values group))
        (query_make_groups nil pre_group q12_group_terms)).
Proof.
  intros D pre_group Hbucket.
  apply Q12_GroupBagOk_of_exact_map_having_true with
    (group_terms := q12_group_terms) (values := q12_group_select_values).
  - exact q12_group_keys_terms.
  - intros sched; split.
    + apply q12_group_dot_keys_runtime_none.
    + intros groups g Hg.
      destruct (Hbucket g Hg) as [Hlab Hsafe].
      assert (Hne : g <> nil).
      { eapply query_make_groups_member_nonempty;
          [exact q12_gterms_nonempty|exact Hg]. }
      unfold q12_group_terms, q12_gterms in *.
      apply q12_group_filter_map_exact; assumption.
Qed.

Lemma Q12_FilterOk_True :
  forall D rows,
    Q12_FilterOk D Queries.scalar_expr_predicate_0 rows rows.
Proof.
  intros D rows sched.
  unfold Q12_FilterOk, Queries.scalar_expr_predicate_0.
  assert (Hkeep : List.filter (fun _ : tuple TNull => true) rows = rows).
  { induction rows as [|r rs IH]; [reflexivity|cbn; f_equal; exact IH]. }
  assert (Hfilt :
    @eval_filter_rows_outcome TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil SExpr_True rows
      (SqlSuccess (List.filter (fun _ : tuple TNull => true) rows))).
  {
    eapply (@filter_rows_success_of_acceptance TNull relname
      (@_basesort TNull D) (@_instance TNull D) unknown3
      NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      nil SExpr_True rows (fun _ => true)).
    intros row Hin; apply scalar_expr_true_acceptance_exact.
  }
  rewrite Hkeep in Hfilt; exact Hfilt.
Qed.

(** Order: canonical sort is always a legal observation. *)
Lemma Q12_OrderOk_canonical :
  forall group_rows,
    Q12_OrderOk group_rows
      (@sort_ordered_rows TNull q12_value_is_null q12_order_keys group_rows).
Proof.
  intro group_rows.
  unfold Q12_OrderOk.
  apply (@order_by_rows_canonical TNull q12_value_is_null).
Qed.

(** * Join bag — canonical observation of CrossJoin bag *)

Lemma Q12_JoinBagOk_canonical :
  forall proj_o proj_l,
    Q12_JoinBagOk proj_o proj_l
      (Febag.elements (Fecol.CBag (CTuple TNull))
         (query_cross_join_bag
            (query_rows_bag proj_o) (query_rows_bag proj_l))).
Proof.
  intros proj_o proj_l.
  unfold Q12_JoinBagOk, query_same_rows_as_bag, query_rows_bag.
  (* mk_bag (elements B) =BE= B *)
  apply Febag.nb_occ_equal; intro x.
  rewrite Febag.nb_occ_mk_bag, Febag.nb_occ_elements.
  reflexivity.
Qed.
(** * Identity-ish table projects (select_list_3 / 4) via ProjectExact *)

Definition q12_orders_dot_values (row : tuple TNull) : list NullValues.value :=
  [ dot TNull row (AttrInt32 "o_orderkey")
  ; dot TNull row (AttrInt32 "o_custkey")
  ; dot TNull row (AttrString "o_orderstatus" (StringChar 1))
  ; dot TNull row (AttrDecimal "o_totalprice" 15 2)
  ; dot TNull row (AttrDate "o_orderdate")
  ; dot TNull row (AttrString "o_orderpriority" (StringChar 15))
  ; dot TNull row (AttrString "o_clerk" (StringChar 15))
  ; dot TNull row (AttrInt32 "o_shippriority")
  ; dot TNull row (AttrString "o_comment" (StringVarcharN 79))
  ].

Definition q12_lineitem_dot_values (row : tuple TNull) : list NullValues.value :=
  [ dot TNull row (AttrInt32 "l_orderkey")
  ; dot TNull row (AttrInt32 "l_partkey")
  ; dot TNull row (AttrInt32 "l_suppkey")
  ; dot TNull row (AttrInt32 "l_linenumber")
  ; dot TNull row (AttrDecimal "l_quantity" 15 2)
  ; dot TNull row (AttrDecimal "l_extendedprice" 15 2)
  ; dot TNull row (AttrDecimal "l_discount" 15 2)
  ; dot TNull row (AttrDecimal "l_tax" 15 2)
  ; dot TNull row (AttrString "l_returnflag" (StringChar 1))
  ; dot TNull row (AttrString "l_linestatus" (StringChar 1))
  ; dot TNull row (AttrDate "l_shipdate")
  ; dot TNull row (AttrDate "l_commitdate")
  ; dot TNull row (AttrDate "l_receiptdate")
  ; dot TNull row (AttrString "l_shipinstruct" (StringChar 25))
  ; dot TNull row (AttrString "l_shipmode" (StringChar 10))
  ; dot TNull row (AttrString "l_comment" (StringVarcharN 44))
  ].

Lemma q12_orders_select_exact_of_dots :
  forall D sched row,
    (forall attr, attr inS Fset.mk_set (A TNull) orders_attrs ->
      attr inS labels TNull row) ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil row)
      (map fst Queries.scalar_select_list_3)
      (q12_orders_dot_values row).
Proof.
  intros D sched row Hlab.
  unfold Queries.scalar_select_list_3, q12_orders_dot_values.
  cbn [map fst].
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull D) (@_instance TNull D)
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched)).
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_orderkey.
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_custkey.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_orderstatus.
  - apply tnull_dot_decimal_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_totalprice.
  - apply tnull_dot_date_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_orderdate.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_priority.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_clerk.
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_shippriority.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact orders_attr_inS_comment.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
Qed.

Lemma q12_lineitem_select_exact_of_dots :
  forall D sched row,
    (forall attr, attr inS Fset.mk_set (A TNull) lineitem_attrs ->
      attr inS labels TNull row) ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil row)
      (map fst Queries.scalar_select_list_4)
      (q12_lineitem_dot_values row).
Proof.
  intros D sched row Hlab.
  unfold Queries.scalar_select_list_4, q12_lineitem_dot_values.
  cbn [map fst].
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull D) (@_instance TNull D)
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched)).
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_orderkey.
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_partkey.
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_suppkey.
  - apply tnull_dot_int32_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_linenumber.
  - apply tnull_dot_decimal_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_quantity.
  - apply tnull_dot_decimal_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_extendedprice.
  - apply tnull_dot_decimal_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_discount.
  - apply tnull_dot_decimal_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_tax.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_returnflag.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_linestatus.
  - apply tnull_dot_date_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_shipdate.
  - apply tnull_dot_date_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_commitdate.
  - apply tnull_dot_date_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_receiptdate.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_shipinstruct.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_shipmode.
  - apply tnull_dot_string_exact; apply interp_dot_env_t;
      apply Hlab; exact lineitem_attr_inS_comment.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
Qed.

Lemma Q12_ProjectTableOk_orders_of_embed :
  forall orders lines,
    Q12_ProjectTableOk (EmbedDB_Q12 orders lines)
      Queries.scalar_select_list_3
      (map EmbedOrderRow orders)
      (map (fun row =>
        project_row Queries.scalar_select_list_3 (q12_orders_dot_values row))
        (map EmbedOrderRow orders)).
Proof.
  intros orders lines sched.
  eapply (@tnull_project_rows_success_of_exact_map
    (@_basesort TNull (EmbedDB_Q12 orders lines))
    (@_instance TNull (EmbedDB_Q12 orders lines)) sched).
  intros row Hin.
  apply in_map_iff in Hin.
  destruct Hin as [o [Heq _]]; subst row.
  apply q12_orders_select_exact_of_dots.
  intros attr Hattr; apply EmbedOrderRow_attr_inS; exact Hattr.
Qed.

Lemma Q12_ProjectTableOk_lines_of_embed :
  forall orders lines,
    Q12_ProjectTableOk (EmbedDB_Q12 orders lines)
      Queries.scalar_select_list_4
      (map EmbedLineRow lines)
      (map (fun row =>
        project_row Queries.scalar_select_list_4 (q12_lineitem_dot_values row))
        (map EmbedLineRow lines)).
Proof.
  intros orders lines sched.
  eapply (@tnull_project_rows_success_of_exact_map
    (@_basesort TNull (EmbedDB_Q12 orders lines))
    (@_instance TNull (EmbedDB_Q12 orders lines)) sched).
  intros row Hin.
  apply in_map_iff in Hin.
  destruct Hin as [l [Heq _]]; subst row.
  apply q12_lineitem_select_exact_of_dots.
  intros attr Hattr; apply EmbedLineRow_attr_inS; exact Hattr.
Qed.

(** * Outer rename project (select_list_0) → EmbedOutRow columns *)

Definition q12_outer_dot_values (row : tuple TNull) : list NullValues.value :=
  [ dot TNull row (AttrString "l_shipmode" (StringChar 10))
  ; dot TNull row (AttrInt64 "high_line_count")
  ; dot TNull row (AttrInt64 "low_line_count")
  ].

Definition q12_outer_row_labels_ok (row : tuple TNull) : Prop :=
  AttrString "l_shipmode" (StringChar 10) inS labels TNull row /\
  AttrInt64 "high_line_count" inS labels TNull row /\
  AttrInt64 "low_line_count" inS labels TNull row.

Lemma q12_outer_select_exact_of_dots :
  forall D sched row,
    q12_outer_row_labels_ok row ->
    @scalar_value_list_exact_at TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched
      (Env.env_t TNull nil row)
      (map fst Queries.scalar_select_list_0)
      (q12_outer_dot_values row).
Proof.
  intros D sched row [Hmode [Hhigh Hlow]].
  unfold Queries.scalar_select_list_0, q12_outer_dot_values.
  cbn [map fst].
  repeat (eapply (@scalar_value_list_exact_cons TNull relname
    (@_basesort TNull D) (@_instance TNull D)
    unknown3 NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error q12_value_is_null sched)).
  - apply tnull_dot_string_exact; apply interp_dot_env_t; exact Hmode.
  - apply tnull_dot_int64_exact; apply interp_dot_env_t; exact Hhigh.
  - apply tnull_dot_int64_exact; apply interp_dot_env_t; exact Hlow.
  - apply (@scalar_value_list_exact_nil TNull relname
      (@_basesort TNull D) (@_instance TNull D)
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error q12_value_is_null sched).
Qed.

Lemma Q12_OuterProjectOk_holds :
  forall D ordered_rows,
    (forall row, In row ordered_rows -> q12_outer_row_labels_ok row) ->
    Q12_OuterProjectOk D ordered_rows
      (map (fun row =>
        project_row Queries.scalar_select_list_0 (q12_outer_dot_values row))
        ordered_rows).
Proof.
  intros D ordered_rows Hlab sched.
  eapply (@tnull_project_rows_success_of_exact_map
    (@_basesort TNull D) (@_instance TNull D) sched).
  intros row Hin.
  apply q12_outer_select_exact_of_dots; apply Hlab; exact Hin.
Qed.
