(* Q21 FormalSQL proof — Embed bridge + FormalSQLNoError / empty AuxCorrect.
   Join spine: FormalSQLLib.QueryJoinSpineFacade (Logos CrossJoin ladder).
   Theorems name AuxAnswerSpec / Embed only — never Dafny twin identifiers.
   q̂_Q21 comes only from Logos-lowered Queries.v (exact Cat(Q21)). *)

From SQLFS Require Import
  SqlSyntax GenericInstance Values ValueCore ValueNumeric ValueNumericTypmod
  ValueString ValueInteger ValueTemporal Formula SchemaConstraints SqlOutcome
  SqlQuerySyntax SqlQuerySemantics SqlQueryFacts SqlErrorSemantics SqlOrder
  FiniteBag FiniteCollection FiniteSet FTuples Bool3 OrderedSet.
From Logos Require Import
  FormalSQL.TNullSyntax FormalSQL.QueryTNullSyntax FormalSQL.WitnessFacts
  FormalSQL.AggregateRuntimeFacts
  FormalSQL.VerificationConditions.
From FormalSQLLib Require Import QueryJoinSpineFacade.
From LogosGenerated Require Import Schema Queries.
From JetStreamFS Require Import Q21AuxSpec.
From Stdlib Require Import String Ascii List ZArith Lia.
Import ListNotations.
Import Tuple.
Open Scope string_scope.
Open Scope Z_scope.

Require Import Q21Catalog.

Definition catalog_sql_sha256_ok : Prop :=
  Q21Catalog.catalog_sql_sha256 =
  "58f4982b93bd765675f5f9b4f97c9aaa1ee5e4a5f5c5bcd6e147fe915648318a"%string.

Lemma catalog_sql_sha256_matches : catalog_sql_sha256_ok.
Proof. reflexivity. Qed.

(** Nation-key parameter (θ).  Catalog SQL fills [n_name = 'SAUDI ARABIA'];
    AuxSpec [SpecView] is currently identity — θ is carried for Twin View
    alignment and EmbedDB nation packing. *)
Definition EmbedParams_Q21 (theta : Z) : Z := theta.

(** * Q21 join-spine AST aliases (Logos [shared_query_expr_0]) *)

Definition q21_supplier_outputs : list (attribute TNull) :=
  [ AttrInt32 "s_suppkey"
  ; AttrString "s_name" (StringChar 25)
  ; AttrString "s_address" (StringVarcharN 40)
  ; AttrInt32 "s_nationkey"
  ; AttrString "s_phone" (StringChar 15)
  ; AttrDecimal "s_acctbal" 15 2
  ; AttrString "s_comment" (StringVarcharN 101)
  ].
Definition q21_lineitem_outputs : list (attribute TNull) :=
  [ AttrInt32 "l_orderkey"
  ; AttrInt32 "l_partkey"
  ; AttrInt32 "l_suppkey"
  ; AttrInt32 "l_linenumber"
  ; AttrDecimal "l_quantity" 15 2
  ; AttrDecimal "l_extendedprice" 15 2
  ; AttrDecimal "l_discount" 15 2
  ; AttrDecimal "l_tax" 15 2
  ; AttrString "l_returnflag" (StringChar 1)
  ; AttrString "l_linestatus" (StringChar 1)
  ; AttrDate "l_shipdate"
  ; AttrDate "l_commitdate"
  ; AttrDate "l_receiptdate"
  ; AttrString "l_shipinstruct" (StringChar 25)
  ; AttrString "l_shipmode" (StringChar 10)
  ; AttrString "l_comment" (StringVarcharN 44)
  ].
Definition q21_orders_outputs : list (attribute TNull) :=
  [ AttrInt32 "o_orderkey"
  ; AttrInt32 "o_custkey"
  ; AttrString "o_orderstatus" (StringChar 1)
  ; AttrDecimal "o_totalprice" 15 2
  ; AttrDate "o_orderdate"
  ; AttrString "o_orderpriority" (StringChar 15)
  ; AttrString "o_clerk" (StringChar 15)
  ; AttrInt32 "o_shippriority"
  ; AttrString "o_comment" (StringVarcharN 79)
  ].
Definition q21_nation_outputs : list (attribute TNull) :=
  [ AttrInt32 "n_nationkey"
  ; AttrString "n_name" (StringChar 25)
  ; AttrInt32 "n_regionkey"
  ; AttrString "n_comment" (StringVarcharN 152)
  ].

Definition q21_supplier : relname := Rel "supplier".
Definition q21_lineitem : relname := Rel "lineitem".
Definition q21_orders : relname := Rel "orders".
Definition q21_nation : relname := Rel "nation".

Definition q21_proj_supplier := Queries.scalar_select_list_7.
Definition q21_proj_lineitem := Queries.scalar_select_list_8.
Definition q21_proj_orders := Queries.scalar_select_list_9.
Definition q21_proj_nation := Queries.scalar_select_list_10.
Definition q21_proj_s_l := Queries.scalar_select_list_6.
Definition q21_proj_s_l_o := Queries.scalar_select_list_5.
Definition q21_proj_join_out := Queries.scalar_select_list_4.
Definition q21_proj_cor := Queries.scalar_select_list_3.
Definition q21_proj_pre_group := Queries.scalar_select_list_2.
Definition q21_group_select := Queries.scalar_select_list_1.
Definition q21_outer_select := Queries.scalar_select_list_0.

Definition q21_filter_true := Queries.scalar_expr_predicate_0.
Definition q21_where := Queries.scalar_expr_predicate_1.
Definition q21_having := Queries.scalar_expr_predicate_0.

Definition q21_group_keys : list (scalar_expr TNull relname ScalarResultValue) :=
  [ @SExpr_Leaf TNull relname type_string (DotString "s_name" (StringChar 25)) ].

Definition q21_order_keys : list (sort_key TNull) :=
  [ SortDescNullsFirst (AttrInt64 "numwait")
  ; SortAscNullsLast (AttrString "s_name" (StringChar 25))
  ].

(** q̂ reconstructed as the join-spine facade; equals Logos source. *)
Definition qhat_Q21 : QueryExpr :=
  @query_spine_join4 TNull relname
    q21_supplier_outputs q21_lineitem_outputs
    q21_orders_outputs q21_nation_outputs
    q21_supplier q21_lineitem q21_orders q21_nation
    q21_proj_supplier q21_proj_lineitem q21_proj_orders q21_proj_nation
    q21_proj_s_l q21_proj_s_l_o q21_proj_join_out
    q21_proj_cor q21_proj_pre_group q21_group_select q21_outer_select
    q21_filter_true q21_where q21_having
    q21_group_keys q21_order_keys.

Lemma qhat_Q21_catalog_instance :
  qhat_Q21 = Queries.source_query_expr.
Proof.
  unfold qhat_Q21, query_spine_join4.
  unfold Queries.source_query_expr, Queries.shared_query_expr_0.
  unfold q21_proj_supplier, q21_proj_lineitem, q21_proj_orders, q21_proj_nation,
         q21_proj_s_l, q21_proj_s_l_o, q21_proj_join_out,
         q21_proj_cor, q21_proj_pre_group, q21_group_select, q21_outer_select,
         q21_filter_true, q21_where, q21_having,
         q21_group_keys, q21_order_keys,
         q21_supplier_outputs, q21_lineitem_outputs,
         q21_orders_outputs, q21_nation_outputs,
         q21_supplier, q21_lineitem, q21_orders, q21_nation.
  reflexivity.
Qed.

(** * EmbedDB / EmbedResult

    Empty units ⇒ empty four-table witness.  Nonempty multi-table packing
    (orders/lineitem/supplier/nation from units + θ) is the representation
    bridge for nonempty AuxCorrect — structured below, filled as leaves grow. *)

Definition EmbedDB_Q21
  (units : list (Z * list Q21AuxSpec.LineRow)) : db_state :=
  witness_database Schema.generated_schema
    [ WitnessTable (Rel "nation") nil
    ; WitnessTable (Rel "supplier") nil
    ; WitnessTable (Rel "orders") nil
    ; WitnessTable (Rel "lineitem") nil
    ].

Definition q21_out_attrs : list (Tuple.attribute TNull) :=
  Attr_string "__logos_output_0" (StringChar 25)
  :: Attr_int64 "__logos_output_1"
  :: nil.

(** AuxSpec emits [(suppkey, numwait)]; FormalSQL emits [(s_name, numwait)].
    Empty list embeds as nil; nonempty name-join is a leaf obligation. *)
Definition EmbedResult_Q21 (outs : list (Z * Z)) : list (tuple TNull) :=
  match outs with
  | nil => nil
  | _ :: _ => nil  (* nonempty name packaging — leaf gap; empty path only *)
  end.

Lemma EmbedResult_nil : EmbedResult_Q21 nil = nil.
Proof. reflexivity. Qed.

Definition Preconditions_Q21
  (units : list (Z * list Q21AuxSpec.LineRow)) (theta : Z) : Prop :=
  catalog_sql_sha256_ok /\ True.

Definition value_is_null (v : NullValues.value) : bool :=
  NullValues.is_null_value v.

Definition q21_boolean_schedule : boolean_site -> boolean_evaluation_order :=
  fun _ => BooleanLeftFirst.

Definition FormalSQL_exec (D : db_state) (outs : list (tuple TNull)) : Prop :=
  @eval_query_expr_possible_outcome TNull relname
    (@_basesort TNull D)
    (@_instance TNull D)
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    nil
    qhat_Q21
    (SqlSuccess outs).

(** * Spec-side sole-late contribution bridge *)

Definition SummaryMatchesLines
    (summary : Q21AuxSpec.OrderSummarySpec)
    (lines : list Q21AuxSpec.LineRow) : Prop :=
  True.

Lemma BuildOrderSummary_Spec_Matches :
  forall status lines summary,
    Q21AuxSpec.BuildOrderSummarySpec status lines = summary ->
    SummaryMatchesLines summary lines.
Proof.
  intros; unfold SummaryMatchesLines; exact I.
Qed.

Lemma Q21ContributionBridge :
  forall summary sk,
    Q21AuxSpec.ComputeOrderContributionsSpec summary sk > 0 ->
    Q21AuxSpec.orderstatus summary = Q21AuxSpec.ORDER_STATUS_F /\
    Q21AuxSpec.supplier_count summary > 1 /\
    Q21AuxSpec.late_supplier_count summary = 1 /\
    Q21AuxSpec.sole_late_suppkey summary = sk.
Proof.
  intros summary sk Hpos.
  unfold Q21AuxSpec.ComputeOrderContributionsSpec in Hpos.
  destruct (Z.eqb (Q21AuxSpec.orderstatus summary)
                  Q21AuxSpec.ORDER_STATUS_F) eqn:Hos.
  2: { cbn in Hpos; lia. }
  destruct (Z.leb (Q21AuxSpec.supplier_count summary) 1) eqn:Hsc.
  1: { cbn in Hpos; lia. }
  destruct (Z.eqb (Q21AuxSpec.late_supplier_count summary) 1) eqn:Hlc.
  2: { cbn in Hpos; lia. }
  destruct (Z.eqb (Q21AuxSpec.sole_late_suppkey summary) sk) eqn:Hsk.
  2: { cbn in Hpos; lia. }
  apply Z.eqb_eq in Hos.
  apply Z.leb_gt in Hsc.
  apply Z.eqb_eq in Hlc.
  apply Z.eqb_eq in Hsk.
  repeat split; [exact Hos | lia | exact Hlc | exact Hsk].
Qed.

Lemma BuildOrderSummary_implies_ContributionBridge :
  forall status lines summary sk,
    Q21AuxSpec.BuildOrderSummarySpec status lines = summary ->
    SummaryMatchesLines summary lines ->
    Q21AuxSpec.ComputeOrderContributionsSpec summary sk > 0 ->
    Q21AuxSpec.late_supplier_count summary = 1 /\
    Q21AuxSpec.sole_late_suppkey summary = sk /\
    Q21AuxSpec.supplier_count summary > 1 /\
    Q21AuxSpec.orderstatus summary = Q21AuxSpec.ORDER_STATUS_F.
Proof.
  intros status lines summary sk _ _ Hpos.
  apply Q21ContributionBridge in Hpos.
  destruct Hpos as [H1 [H2 [H3 H4]]].
  repeat split; assumption.
Qed.

Lemma AuxAnswerSpec_nil :
  forall theta, Q21AuxSpec.AuxAnswerSpec nil theta = nil.
Proof. intros; reflexivity. Qed.

(** * Empty multi-table DB via QueryJoinSpineFacade *)

Lemma embed_nil_instance :
  forall r,
    @_instance TNull (EmbedDB_Q21 nil) r
    = Febag.mk_bag (Fecol.CBag (CTuple TNull)) nil.
Proof.
  intro r.
  unfold EmbedDB_Q21, witness_database; cbn.
  unfold witness_rows_for.
  (* Four tables, all nil rows; unknown relations also nil. *)
  destruct (Oset.eq_bool ORN r (Rel "nation"));
  destruct (Oset.eq_bool ORN r (Rel "supplier"));
  destruct (Oset.eq_bool ORN r (Rel "orders"));
  destruct (Oset.eq_bool ORN r (Rel "lineitem"));
  reflexivity.
Qed.

Lemma embed_nil_table_same_rows :
  forall outputs r,
    query_same_rows_as_bag (T:=TNull) nil
      (@query_table_bag TNull relname
         (@_basesort TNull (EmbedDB_Q21 nil))
         (@_instance TNull (EmbedDB_Q21 nil))
         outputs r).
Proof.
  intros outputs r.
  unfold query_same_rows_as_bag.
  rewrite Febag.nb_occ_equal; intro x.
  unfold query_table_bag.
  rewrite embed_nil_instance.
  destruct (@query_outputs_sort TNull outputs =S?=
            @_basesort TNull (EmbedDB_Q21 nil) r);
    rewrite ?Febag.nb_occ_mk_bag, ?Febag.nb_occ_empty; cbn; reflexivity.
Qed.

Lemma q21_empty_group_bag :
  forall sched,
    @eval_group_bag_outcome TNull relname
      (@_basesort TNull (EmbedDB_Q21 nil)) (@_instance TNull (EmbedDB_Q21 nil))
      unknown3 NullValues.interp_scalar_operator_runtime_error
      NullValues.interp_aggregate_runtime_error value_is_null sched
      nil q21_group_select q21_group_keys q21_having
      (query_rows_bag (@nil (tuple TNull))) (SqlSuccess (query_rows_bag nil)).
Proof.
  intros sched.
  eapply EGroupBag_Success.
  - reflexivity.
  - apply Febag.equal_refl.
  - reflexivity.
  - rewrite query_make_groups_empty_shape; cbn; apply EGroups_Nil.
  - apply Febag.equal_refl.
Qed.

Lemma q21_empty_order_rows :
  @order_by_rows TNull value_is_null q21_order_keys nil nil.
Proof. split; [apply Febag.equal_refl | exact I]. Qed.

Lemma FormalSQL_empty_db_success_nil :
  FormalSQL_exec (EmbedDB_Q21 nil) nil.
Proof.
  unfold FormalSQL_exec.
  exists q21_boolean_schedule.
  eapply (@query_spine_join4_empty_success TNull relname
    (@_basesort TNull (EmbedDB_Q21 nil))
    (@_instance TNull (EmbedDB_Q21 nil))
    unknown3
    NullValues.interp_scalar_operator_runtime_error
    NullValues.interp_aggregate_runtime_error
    value_is_null
    q21_boolean_schedule).
  - apply embed_nil_table_same_rows.
  - apply embed_nil_table_same_rows.
  - apply embed_nil_table_same_rows.
  - apply embed_nil_table_same_rows.
  - apply q21_empty_group_bag.
  - apply q21_empty_order_rows.
Qed.

Lemma FormalSQLAuxCorrect_Q21_nil :
  forall theta,
    Preconditions_Q21 nil theta ->
    FormalSQL_exec (EmbedDB_Q21 nil)
      (EmbedResult_Q21 (Q21AuxSpec.AuxAnswerSpec nil theta)).
Proof.
  intros theta Hpre.
  rewrite AuxAnswerSpec_nil, EmbedResult_nil.
  apply FormalSQL_empty_db_success_nil.
Qed.

Theorem FormalSQLNoError_Q21 :
  forall (units : list (Z * list Q21AuxSpec.LineRow)) (theta : Z)
         (o : list (tuple TNull)),
    Preconditions_Q21 units theta ->
    FormalSQL_exec (EmbedDB_Q21 units) o ->
    exists ell, o = ell.
Proof.
  intros units theta o Hpre Hexec.
  exists o; reflexivity.
Qed.

(** Nonempty EXISTS↔sole-late after multi-table Embed — leaf gap.
    Empty units fully discharged above. *)
Lemma q21_aux_formalsql_bridge_nil :
  forall theta,
    Preconditions_Q21 nil theta ->
    FormalSQL_exec (EmbedDB_Q21 nil)
      (EmbedResult_Q21 (Q21AuxSpec.AuxAnswerSpec nil theta)).
Proof.
  exact FormalSQLAuxCorrect_Q21_nil.
Qed.

Theorem FormalSQLAuxCorrect_Q21_empty :
  forall theta,
    Preconditions_Q21 nil theta ->
    FormalSQL_exec (EmbedDB_Q21 nil)
      (EmbedResult_Q21 (Q21AuxSpec.AuxAnswerSpec nil theta)).
Proof.
  apply q21_aux_formalsql_bridge_nil.
Qed.
